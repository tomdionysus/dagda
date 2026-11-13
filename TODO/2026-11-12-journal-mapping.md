# Macha store journal lines against Dagda

Read-only audit, 2026-11-12. Macha tree as checked out at `../macha` (port branch; store code as 98b816d
plus call deadlines). Dagda at `feature/manifest-lock` (2413486). Line numbers are the statement's first
line. "Dagda" paths are under `dagda/`; Macha paths under `../macha/src/`.

## 1. Summary

145 Macha logging statements in the listed files, plus two in headers the brief's globs did not name
but that are store code moving to Dagda (`storage/io_pressure.hpp:153`, `cluster/data_work.hpp:311`;
both listed as removed in `TODO/2026-11-05-second-cutover.md:95`, `:105`).

| Verdict | Count |
|---|---|
| SAME | 65 |
| REWORDED | 23 |
| MISSING | 39 |
| N/A | 18 |
| Total | 145 |

By level, MISSING is 0 error, 1 warn, 2 info, 26 debug, 10 trace; REWORDED is 2 error, 3 warn,
3 info, 15 debug.

Warn and error lines that are not SAME:

| Macha | Level | Verdict | What changes |
|---|---|---|---|
| `cluster/distributed_store.cpp:580` | WARN | MISSING | `local storage durability barrier failed: <e>`. Dagda catches the same failure silently (`store/distributed_store.cpp:1430-1432`). The pool's own `storage durability barrier failed <path>: <e>` (SAME, `store/storage_pool.cpp:606`) still fires when the store throws; the case that throws outside the pool's try (`placement is no longer available`, `store/storage_pool.cpp:611`) now logs nothing at all. |
| `cluster/distributed_store.cpp:2377` | WARN | REWORDED | Tail `; the live namespace references it and no peer would supply it` becomes `; what is referenced names it and no peer would supply it` (`store/distributed_store.cpp:2568`). The head of the line, the one a grep for "repair cannot source" finds, is unchanged. |
| `cluster/distributed_store.cpp:2395` | WARN | REWORDED | `repair found a local object it cannot read back object=<id> local_unreadable_total=<n>; the store listed it but the read failed`, at most once a minute (`:2344`, `:2387-2392`), becomes DEBUG `repair found a local object it cannot read back object=<id>` on every occurrence (`store/distributed_store.cpp:2804`). Gone from a default INFO journal. |
| `cluster/local_state.cpp:121` | WARN | REWORDED | `node data storage degraded: no disk online of <n>; running without one until it returns` becomes `data storage degraded: every configured backend is offline` (`node.cpp:218`). |
| `cluster/local_state.cpp:127` | ERROR | REWORDED | `node data storage recovery failed: <e>` becomes `node refused: <e>` (`node.cpp:95`), for every plane Dagda opens. |
| `cluster/local_state.cpp:183` | ERROR | REWORDED | `node local state recovery failed: <e>`: same `node refused: <e>` for the planes Dagda opens; the metadata plane stays Macha's until 8c. |
| `ledger/node_horizon_builder.cpp:72` | WARN | N/A | `retention release horizon namespace tree unavailable root=<id> error=<e>`. The file is removed at 8a and the namespace walk becomes Macha's provider (`TODO/2026-11-05-second-cutover.md:364`, `:378-379`). Dagda's builder only logs DEBUG `retention release horizon unavailable: <e>` when the provider throws (`ledger/node_horizon_builder.cpp:45`). If Macha's provider does not keep the WARN, it is lost. |
| `service/availability_service.cpp:321`, `:570` | WARN | N/A | `availability: ignoring <path>: <e>` and `availability: cannot keep the survey: <e>` belong to the path table and `last-survey.bin`, which stay Macha's (`TODO/2026-11-05-second-cutover.md:132-135`). |

## 2. How a Dagda line reads in Macha's journal

- Dagda logs through `dannan::Context::logger(scope)`, which is `ScopedLogger{_config.log, scope}`
  (`../dannan/dannan/node.h:231`). `ScopedLogger::log` checks `sink->enabled(level)`, then calls
  `sink->write(level, scope, message)` (`../dannan/dannan/status/log.h:93-95`). A child scope is
  `parent + "." + part` (`../dannan/dannan/status/log.cpp:163`).
- Macha installs `macha_log_sink()` as that sink (`cluster/cluster.cpp:146`). Its `write` builds
  `scope + ": " + message` when the scope is non-empty and passes it to `Log::emit`
  (`cluster/dannan_bridge.cpp:221-229`). `Log::emit` writes through the installed logger with no
  second filter (`log.cpp:99-101`). The only logger is `ConsoleLogger` (`service/main.cpp:22`,
  `service/service_config.cpp:40`), which prints `<YYYY-MM-DD HH:MM:SSZ> <LEVEL> <text>`
  (`log.cpp:66`).
- Levels map one to one. The two enums have the same values (`all=0, debug=1, info=2, warn=3,
  error=4`: `../dannan/dannan/status/log.h:19`, `log.hpp:14-20`), and the sink casts between them
  (`cluster/dannan_bridge.cpp:212-214`). `enabled` defers to `Log::enabled` (`:218-220`), so
  Macha's configured level filters Dagda's lines. Dagda's `trace` is `LogLevel::all` and prints as
  `ALL`, as Macha's `Log::trace` does (`log.cpp:21-22`).
- **So the scope does appear in the text.** A line marked SAME below differs from Macha's in one
  way: the message is preceded by `<scope>: `. For example Macha's
  `2026-11-06 03:02:13Z DEBUG storage accounting scan begin path=.../data` becomes
  `... DEBUG dagda.pool.backend: storage accounting scan begin path=.../data`. A substring grep for
  the message still matches; a grep anchored on `<LEVEL> <message>` does not.
- The scopes (each cited where it is set):

| Scope | Lines | Set at |
|---|---|---|
| `dagda.node` | node open/stop steps, degraded, refused, held-ledger verify, local stages | `node.cpp:81` |
| `dagda.pool` | backend probe/online/offline, pool read/write/barrier/compaction lines | `node.cpp:214` |
| `dagda.pool.backend` | every LocalStore line of a DATA backend (accounting, presence index, packs, held ledger) | `store/storage_pool.cpp:355` |
| `dagda.pool.pressure` | `DATA device pressure onset/released` | `store/storage_pool.cpp:153` |
| `dagda.control` | the control store's LocalStore lines **and** `control object unavailable` / `control object read` | `node.cpp:238`, `store/control_objects.cpp:18` |
| `dagda.cache` | persistent cache | `node.cpp:245` |
| `dagda.credit` | `DATA credit wait abandoned` | `node.cpp:228` |
| `dagda.claims` | retention ledger, its sealed journal | `node.cpp:249` |
| `dagda.references` | reference counts | `node.cpp:255` |
| `dagda.gc` | collector lines, unreferenced-since | `node.cpp:341-343`, `:460` |
| `dagda.horizon` | release horizon | `node.cpp:489` |
| `dagda.store` | distributed store | `store/distributed_store.cpp:502` |
| `dagda.repair` | repair stage (slow-stage trace) | `store/repair.cpp:61` |
| `dagda.server` | storage server | `store/storage_server.cpp:59` |
| `dagda.availability` | survey | `store/availability.cpp:103` |
| `dagda.heads` | heads (metadata) | `heads/exchange.cpp:90`, `heads/spaces.cpp:46-47` |

  Not verified: the scope a held ledger's own lines get (`_logger.child("ledger")`,
  `store/local_store.cpp:135`, so `dagda.pool.backend.ledger`), and where `SealedJournal` and
  `ObjectTrie` get theirs; I did not trace every constructor.
- Variable parts that are rendered differently even where the template is SAME:
  - Peer errors. Macha's `error=` and `last_failure` carry the RPC exception's text; Dagda's carry
    `outcome_text` (`<status> <code>: <message>`, `reply type <n>` or Dannan's failure string,
    `store/distributed_store.cpp:52-57`) or `outcome.failure` (`store/control_objects.cpp:33`).
    I did not compare Macha's RPC exception strings with Dannan's word for word.
  - Object ids: both are full lowercase hex (`types.cpp:46-48`; `store/object_id.cpp:14`), so
    `control object unavailable id=` reads the same (Macha uses `hex(id.bytes)`,
    `cluster/control_objects.cpp:60`).

## 3. Line by line

S = SAME, R = REWORDED, M = MISSING. Texts are the concatenations with variables in `<>`.
"(stage)" names the cutover stage at which the line leaves Macha where it is not step S
(`TODO/2026-11-10-dagda-capabilities.md`, "What the port must do", maintenance: Macha's maintenance loop keeps running until 8c, calling Dagda's stages
as steps; the reachability provider and collector move at 8a; heads at 8c).

### storage/local_store.cpp (all to `store/local_store.cpp`, scope `dagda.pool.backend` or `dagda.control`)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :508 | DEBUG | `storage accounting restored path=<root> used=<n>` | :149 | DEBUG | S |
| :513 | INFO | `storage accounting estimate path=<root> used=<n> (dirty checkpoint; admitting writes while the scan reconciles)` | :153 | INFO | S |
| :546 | WARN | `storage accounting scan failed path=<root> error=<e>` | :191 | WARN | S |
| :551 | WARN | `storage accounting scan failed path=<root> error=<unknown>` | :195 | WARN | S |
| :606 | INFO | `storage held ledger seeded path=<root> objects=<n> elapsed_ms=<ms>` | :265 | INFO | S |
| :829 | WARN | `storage pruned an empty object file id=<id> path=<p>` | :414 | WARN | S |
| :1001 | WARN | `truncated incomplete pack tail path=<file>` | :645 | WARN | S |
| :1030 | WARN | `truncated undecodable pack tail path=<file> offset=<o> bytes=<n> zero_header=<0/1>` | :671 | WARN | S |
| :1039 | ERROR | `skipped unreadable pack region path=<file> offset=<o> bytes=<n> zero_header=<0/1> (objects recorded there are absent from this backend)` | :678 | ERROR | S |
| :1052 | WARN | `truncated incomplete packed object path=<file>` | :689 | WARN | S |
| :1909 | WARN | `cannot remove compacted source pack path=<file>[ error=<msg>]` | :1496 | WARN | S |
| :2066 | WARN | `storage held ledger corrected path=<root> recorded=<n> lost=<n>` | :1603 | WARN | S |
| :2112 | DEBUG | `storage presence index warmed path=<root> objects=<n> elapsed_ms=<ms>` | :1645 | DEBUG | S |
| :2118 | WARN | `storage presence index walk failed path=<root> error=<msg> retry_s=<s>` | :1650 | WARN | S |
| :2130 | DEBUG | `storage accounting scan begin path=<root>` | :1661 | DEBUG | S |
| :2177 | WARN | `storage accounting scan failed path=<root> error=<msg>` | :1704 | WARN | S |
| :2191 | WARN | `storage accounting baseline durability failed path=<root> error=<e>` | :1715 | WARN | S |
| :2239 | DEBUG | `storage accounting scan complete path=<root> used=<n>` | :1756 | DEBUG | S |

### storage/persistent_cache.cpp, retention.cpp, sealed_journal.cpp, unreferenced_since.cpp, io_pressure.hpp

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| persistent_cache.cpp:83 | WARN | `persistent cache disabled: <e>` | store/persistent_cache.cpp:46 | WARN | S |
| persistent_cache.cpp:229 | DEBUG | `persistent cache write: <e>` | store/persistent_cache.cpp:178 | DEBUG | S |
| persistent_cache.cpp:253 | DEBUG | `persistent cache read: <e>` | store/persistent_cache.cpp:201 | DEBUG | S |
| persistent_cache.cpp:353 | DEBUG | `persistent metadata cache write: <e>` | heads/cache_seed.cpp:48 | DEBUG | S |
| persistent_cache.cpp:387 | DEBUG | `persistent metadata cache read: <e>` | heads/cache_seed.cpp:66 | DEBUG | S |
| retention.cpp:243 | DEBUG | `cannot remove retention checkpoint files: <msg>` | ledger/retention.cpp:48 | DEBUG | S |
| retention.cpp:398 | INFO | `retention claims moved into the ledger data=<n> control=<n>` (first start, including a fresh node: golden a:20, b:22) | ledger/retention.cpp:257 | INFO | R: `retention claims ledger created`. The legacy checkpoint move is gone by ruling (`TODO/2026-11-05-second-cutover.md:349-351`); the fresh-node event remains with new text. |
| sealed_journal.cpp:135 | WARN | `<what> recovered path=<p> offset=<o> reason=<e>`; what is `retention journal` (retention.cpp:445) or `object trie journal` (ledger/object_trie.cpp:507) | ledger/sealed_journal.cpp:157 | WARN | S (same names: ledger/retention.cpp:298, ledger/object_trie.cpp:448) |
| unreferenced_since.cpp:55 | WARN | `unreferenced-since: ignoring <path>: <e>` | ledger/unreferenced_since.cpp:84 | WARN | S |
| unreferenced_since.cpp:109 | WARN | `unreferenced-since: cannot write <path>: <e>` | ledger/unreferenced_since.cpp:132 | WARN | S |
| io_pressure.hpp:153 | INFO | `DATA device pressure onset\|released slowdown_percent=<n> last_percent=<n> last_us=<n> last_bytes=<n>` | store/io_pressure.cpp:58 | INFO | S |

### storage/storage_pool.cpp (to `store/storage_pool.cpp`, scope `dagda.pool`)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :143 | WARN | `storage backend offline <path>: <reason>` | :234 | WARN | S (same once-per-reason rule, :223) |
| :172 | DEBUG | `storage backend probe begin path=<path>` | :260 | DEBUG | S |
| :280 | INFO | `storage backend online <path> elapsed_ms=<ms> accounting=ready\|reconciling` | :405 | INFO | S |
| :422 | DEBUG | `storage write failed <path>: <e>` | :532 | DEBUG | S |
| :469 | DEBUG | `storage deferred write failed <path>: <e>` | :575 | DEBUG | S |
| :503 | WARN | `storage durability barrier failed <path>: <e>` | :606 | WARN | S |
| :558 | TRACE | `DIAG storage-get window_ms=<n> gets=<n> bytes=<n> avg_ms=<n> max_ms=<n>` | :653 | TRACE | S |
| :567 | WARN | `cannot read local object <id> on <path>; keeping it: <e>` | :707 | WARN | S |
| :599 | TRACE | `DIAG backend-get path=<p> id=<id> result=<0/1> bytes=<n> ms=<n>` | :738 | TRACE | S |
| :607 | WARN | `discarding corrupt local object <id> on <path>: <e>` | :749 | WARN | S. Note: before the backend's key is checked Dagda writes `unreadable local object <id> on <path> kept (key unchecked): <e>` instead (:746), so not every corrupt object now yields this line. |
| :612 | DEBUG | `removing corrupt local object: <e>` | :752 (silent catch) | - | M |
| :614 | DEBUG | `removing corrupt local object: unknown error` | :752 | - | M |
| :668 | WARN | `storage durability reassertion failed <path>: <e>` | :799 | WARN | S |
| :750 | DEBUG | `removing local object: <e>` | :867 (silent) | - | M |
| :752 | DEBUG | `removing local object: unknown error` | :867 | - | M |
| :774 | DEBUG | `listing local objects: <e>` | :886 (silent) | - | M |
| :776 | DEBUG | `listing local objects: unknown error` | :886 | - | M |
| :966 | DEBUG | `rebalancing to preferred backend: <e>` | :1051 (silent) | - | M |
| :968 | DEBUG | `rebalancing to preferred backend: unknown error` | :1051 | - | M |
| :980 | DEBUG | `removing rebalanced duplicate: <e>` | :1061 (silent) | - | M |
| :982 | DEBUG | `removing rebalanced duplicate: unknown error` | :1061 | - | M |
| :1019 | WARN | `removing corrupt local object <id> on <path>: <e>` (scrub) | :1098 | WARN | S (same key-unchecked caveat, :1095) |
| :1024 | DEBUG | `removing corrupt local object: <e>` | :1101 (silent) | - | M |
| :1026 | DEBUG | `removing corrupt local object: unknown error` | :1101 | - | M |
| :1100 | DEBUG | `garbage collection skipped local object <id> on <path>: <e>` | :1162 | DEBUG | S |
| :1159 | WARN | `DATA pack compaction deferred path=<p> error=<e>` | :1215 | WARN | S |

### ledger/*.cpp

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| availability.cpp:101 | DEBUG | `availability: a peer could not finish a trie diff: <e>` | store/availability.cpp:189 | DEBUG | R: no line of its own; the reason is now ` why=<why>` on the `availability diff failed` line |
| node_horizon_builder.cpp:59 | DEBUG | `retention release horizon catalogue unavailable root=<id> error=<e>` | (Macha's provider, 8a) | - | N/A |
| node_horizon_builder.cpp:72 | WARN | `retention release horizon namespace tree unavailable root=<id> error=<e>` | (Macha's provider, 8a); Dagda wraps any provider failure as `retention release horizon unavailable: <e>`, ledger/node_horizon_builder.cpp:45 | DEBUG | N/A (see section 1) |
| node_horizon_builder.cpp:109 | DEBUG | `retention release horizon walks the namespace: <e>` | (Macha's provider, 8a) | - | N/A |
| reference_counts.cpp:105 | WARN | `reference counts state unreadable path=<dir>/state error=<e>` | ledger/reference_counts.cpp:102 | WARN | S |

### cluster/data_work.hpp

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :311 | WARN | `DATA credit wait abandoned after <ms> ms with no release anywhere: class=<frame> bytes=<n> used=<n>/<cap> lower_active=<n>/<n> waiting_viewers=<n> waiting_loaders=<n>` | store/data_work.cpp:158, written at :164 | WARN | S (frame names come from `dannan::sched::frame_type_name`; not compared with Macha's `frame_type_name`) |

### cluster/distributed_store.cpp (to `store/distributed_store.cpp`, scope `dagda.store`)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :154 | TRACE | `DIAG object-put id=<id> bytes=<n> ms=<n>` | none | - | M |
| :282 | DEBUG | `object write quorum id=<id> bytes=<n> replicas=<n> required=<n> minimum=<n> success=<n> local_ms=<n> remote_max_ms=<n> total_ms=<n> result=<0/1>` (writes of 500 ms or more) | none (`object write landed nowhere id=<id> level=<l>`, :1025, is a different event) | - | M |
| :372 | DEBUG | `object write made no progress within budget id=<id> budget_ms=<n> success=<n> sought=<n>` | :914 | DEBUG | S (Dagda's `sought=` is `floor`, Macha's `need`; not checked whether they mean the same number) |
| :395 | DEBUG | `object write stalled; spilling id=<id> peer=<12 hex> no_progress_ms=<n>` | :897 | DEBUG | R: no ` no_progress_ms=` |
| :580 | WARN | `local storage durability barrier failed: <e>` | none (:1430-1432 silent) | - | M |
| :725 | INFO | `object durability re-derived after incarnation change reasserted=<n> absent=<n> peers=<n>` | none (the probe path :1377-1410 logs nothing) | - | M |
| :761 | DEBUG | `object durability quorum unavailable id=<id> required=<n> durable=<n> transient=yes\|no<detail>` | none | - | M |
| :830 | DEBUG | `retention claim peer=<host> error=<e>` | :2079 | DEBUG | S (error is `outcome_text`) |
| :890 | DEBUG | `DATA retention barrier ids=<n> nodes=<n> total_ms=<n> scan_ms=<n> short=<n> fallback_claims=<n> under_claimed=<n>` (250 ms or more) | none in store; heads' `metadata retention barrier dot=... data_ms=... control_ms=...` (heads/space.cpp:479) covers the whole barrier | - | M (unsure whether the heads line is meant as its replacement) |
| :1125 | DEBUG | `CONTROL retention claim objects=<n> sought=<n> tried=<n> retained=<n> total_ms=<n>` (250 ms or more) | none; as above | - | M |
| :1168 | DEBUG | `repair position not saved: <e>` | none | - | N/A: repair cursors gone by ruling "repair diff only" (`TODO/2026-11-05-second-cutover.md:344-348`). Unsure that this cursor is the one ruled out. |
| :1313 | DEBUG | `prompt replication id=<id> error=<e>` | :1619 | DEBUG | S |
| :1436 | DEBUG | `object read: <e>` | :1845 | DEBUG | R: `object read <host>: <e>`, and only on a reply that fails to decode; a call with no reply logs nothing |
| :1696 | DEBUG | `remote object scheduling <id>: <e>` | none | - | M |
| :1699 | DEBUG | `remote object scheduling <id>: unknown error` | none | - | M |
| :1737 | DEBUG | `playback object read source=<s> stripe=<n> id=<id> result=<0/1> bytes=<n> elapsed_ms=<n>` | none | - | M |
| :1761 | TRACE | `DIAG object-get id=<id> source=owned bytes=<n> ms=<n>` | none | - | M |
| :1790 | TRACE | `DIAG object-get id=<id> source=cache bytes=<n> ms=<n>` | none | - | M |
| :1808 | TRACE | `DIAG object-get id=<id> source=remote result=<0/1> bytes=<n> ms=<n>` | none | - | M |
| :1878 | DEBUG | `retention presence batch peer=<host> error=<e>` | :2000 | DEBUG | R: `retention presence batch: <e>`, no peer |
| :1911 | DEBUG | `retention presence batch peer=<host> error=<e>` | :2000 | DEBUG | R: as above |
| :1952 | DEBUG | `repair presence batch peer=<host> error=<e>` | :2482 | DEBUG | R: `validated presence peer=<host> error=<e>`, decode failures only |
| :2006 | DEBUG | `repair presence batch peer=<host> error=<e>` | :2482 | DEBUG | R: as above |
| :2100 | DEBUG | `universal object write <host>: <e>` | :2495 | DEBUG | S |
| :2133 | DEBUG | `CONTROL presence probe peer=<host> error=<e>; sending the whole graph` | :2174 | DEBUG | S |
| :2140 | DEBUG | `CONTROL graph peer=<host> referenced=<n> missing=<n>` | none | - | M |
| :2167 | DEBUG | `CONTROL object store peer=<host> error=<e>` | :2199 | DEBUG | S (one site in Dagda for Macha's two; error is `outcome_text`) |
| :2191 | DEBUG | `CONTROL object store peer=<host> error=<e>` | :2199 | DEBUG | S |
| :2221 | DEBUG | `CONTROL offer peer=<host> error=<e>` | none | - | M |
| :2245 | DEBUG | `cache write-through skipped object=<id> error=<e>` | :586 | DEBUG | S |
| :2318 | DEBUG | `object delete <host>: <e>` | :2517 | DEBUG | S |
| :2377 | WARN | `repair cannot source an object this node should own object=<id> unsourceable_total=<n>; the live namespace references it and no peer would supply it` | :2568 | WARN | R: tail `; what is referenced names it and no peer would supply it`. Rate limit the same, once a minute (Macha :2344, :2370-2374; Dagda :2562). |
| :2395 | WARN | `repair found a local object it cannot read back object=<id> local_unreadable_total=<n>; the store listed it but the read failed` | :2804 | DEBUG | R: DEBUG, no total, no tail, no rate limit |
| :2868 | DEBUG | `repair push peer=<host> error=<e>` | :2846 | DEBUG | S (error is `outcome_text`) |
| :3115 | DEBUG | `opportunistic persistence skipped object=<id> reason=retained_memory bytes=<n> cache=<0/1> promote=<0/1>` | none | - | M (Dagda has no retained-memory gate; may be N/A by design, unverified) |
| :3126 | DEBUG | `opportunistic persistence skipped object=<id> reason=queue_full queued_bytes=<n>` | :1901 | DEBUG | R: no ` queued_bytes=` |
| :3180 | DEBUG | `opportunistic persistence skipped object=<id> reason=data_credit` | none | - | M |
| :3188 | DEBUG | `opportunistic persistence skipped object=<id> reason=cache_put_failed` | :1934 | DEBUG | S |

### cluster/storage_server.cpp, control_objects.cpp

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| storage_server.cpp:53 | TRACE | `DIAG node-stage stage=storage-refresh elapsed_ms=<n>` | none | - | M |
| storage_server.cpp:310 | DEBUG | `object durability barrier refused: epoch changed expected=<8 hex> current=<8 hex> domain=<n> generation=<n>` | none (store/storage_server.cpp:300 refuses silently) | - | M |
| storage_server.cpp:331 | INFO | `object durability re-derived after epoch change present=<n>/<n> expected=<8 hex> current=<8 hex>` | store/storage_server.cpp:304 | INFO | R: ends at `present=<n>/<n>`; no expected/current epochs |
| control_objects.cpp:46 | DEBUG | `control object read <host>: integrity failure` | store/control_objects.cpp:44 | DEBUG | S |
| control_objects.cpp:56 | DEBUG | `control object read <host>: <e>` | store/control_objects.cpp:52 | DEBUG | S for decode failures. A call that gets no reply or a refusal now logs nothing per peer (:32-40); Macha's transport errors threw into this catch and were logged. |
| control_objects.cpp:60 | WARN | `control object unavailable id=<hex> peers_asked=<n> last_failure=<f>` | store/control_objects.cpp:56 | WARN | S. Same template, same initial `no active peer`. `last_failure` for a refusal is `<host>: <refusal code>` or `<host>: reply type <n>` (:38) where Macha had `<host>: <message type name>` (:37); for no reply, Dannan's failure string (:33). |

### cluster/local_state.cpp (replaced by a facade over `dagda::Node`; `TODO/2026-11-05-second-cutover.md:118-120`)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :102 | INFO | `data io pressure gate enabled expected_ms=<n>+<n>/MiB slowdown_percent=<n> release_percent=<n> outlier_percent=<n> min_background=<n>` (golden a:15, b:15) | none; Dagda writes only `open admission` (node.cpp:222 via :106) | - | M (unless Macha's facade writes it) |
| :117 | INFO | `node data storage ready used=<n> capacity=<n>[ (hosts no extents)]` (golden a:16, b:16) | node.cpp:363 | INFO | R: `node open: domain <name> (<id>), <n> backends online, used <n> of <n>`, written once the whole node is open, not when the DATA plane is |
| :121 | WARN | `node data storage degraded: no disk online of <n>; running without one until it returns` | node.cpp:218 | WARN | R: `data storage degraded: every configured backend is offline` |
| :127 | ERROR | `node data storage recovery failed: <e>` | node.cpp:95 | ERROR | R: `node refused: <e>` |
| :178 | INFO | `node metadata ready generation=<n>` (golden a:22, b:24) | none | - | N/A until 8c (the replica stays in Macha's LocalState). Dagda has no counterpart after 8c. |
| :183 | ERROR | `node local state recovery failed: <e>` | node.cpp:95 | ERROR | R: `node refused: <e>` for the planes Dagda opens |

### service/availability_service.cpp (survey to `store/availability.cpp`, scope `dagda.availability`)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :321 | WARN | `availability: ignoring <path>: <e>` | stays Macha (path table) | - | N/A |
| :484 | DEBUG | `availability diff done\|failed peer=<12 hex> questions=<n> reply_bytes=<n> waited_ms=<n>` | :189 | DEBUG | R: no ` waited_ms=`; adds ` why=<why>` when failed |
| :553 | DEBUG | `availability surveyed generation=<n> referenced=<n> local=<n> unavailable=<n> unknown=<n> peers=<n> peers_failed=<n> rounds=<n> prefixes_asked=<n> table=<t> method=diff\|gained` | :215 | DEBUG | R: `... referenced=<n> missing_here=<n> unavailable=... rounds=<n> method=...`: `local=` becomes `missing_here=` (its complement), no `prefixes_asked=`, no `table=` (the table stays Macha's) |
| :570 | WARN | `availability: cannot keep the survey: <e>` | stays Macha (path table) | - | N/A |

### service/maintenance.cpp (store parts)

| Macha | Lvl | Text | Dagda | Lvl | V |
|---|---|---|---|---|---|
| :76 | DEBUG | `maintenance: scrub schedule persistence unavailable: <e>` | node.cpp:609 | DEBUG | R: `scrub schedule not kept: <e>` |
| :87 | TRACE | `DIAG maintenance-stage stage=<s> elapsed_ms=<n>[ <detail>]` | store/slow_stage.h:31 | TRACE | S for the stages Dagda runs (garbage-collect ledger/collector.cpp:249, local-rebalance node.cpp:668, scrub :778, network-repair store/repair.cpp:194). Details not compared. |
| :129, :131, :133 | DEBUG | `shutdown: service maintenance request_stop\|joining\|joined` | Macha's loop | - | N/A (loop stays Macha's until 8c) |
| :514 | DEBUG | `metadata repair deferred; continuing non-destructive maintenance: <e>` (golden a:36) | heads/exchange.cpp:1372 | DEBUG | S (8c) |
| :530 | DEBUG | `metadata repair deferred; continuing non-destructive maintenance` | heads/exchange.cpp:1372 | DEBUG | R (8c): Dagda always appends `: <why>` |
| :553 | DEBUG | `catalogue sync: <e>` | Macha's catalogue | - | N/A |
| :604 | DEBUG | `availability survey deferred: <e>` | Macha's loop calls the survey until 8c | - | N/A until 8c; after 8c probably folded into `repair pass: <e>` (node.cpp:906, :950), unverified |
| :655 | TRACE | `DIAG maintenance-inventory generation=<n> entries=<n> extents=<n> live=<n> garbage=<n> stale_garbage=<n> elapsed_ms=<n>` | none (ledger/collector.cpp:130 is a trace callback, not a log: :57-59) | - | M (8a) |
| :843 | TRACE | `maintenance: repair yielded to foreground I/O` | Macha's loop | - | N/A until 8c; Dagda keeps counters only (store/repair.cpp:142-143) |
| :853 | TRACE | `maintenance: repair quiescent; waiting for an event` | Macha's loop | - | N/A until 8c; as above |
| :971 | DEBUG | `catalogue control GC removed=<n>` | ledger/collector.cpp:194 | DEBUG | R (8a): `control GC removed=<n>` |
| :986 | DEBUG | `garbage collection sweep node=<6 hex> enabled\|skipped: <reason>` (golden a:33,34,45; b:35,41,42) | ledger/collector.cpp:211 | DEBUG | R (8a): `garbage collection sweep enabled\|skipped: <reason>`, no ` node=`. Reasons are the same strings (contract/gates.cpp:24, :34; Macha contract/gates.cpp:26-27, :37). Same change-only rule. |
| :999 | DEBUG | `retention released DATA claims node=<6 hex> count=<n>` | none (trace callback only, ledger/collector.cpp:222) | - | M (8a) |
| :1004 | DEBUG | `retention release skipped: horizon incomplete` (once) | ledger/collector.cpp:225 (once) | DEBUG | S |
| :1063 | DEBUG | `garbage collection sweep node=<6 hex> objects=<n> bytes=<n> deferred=<0/1> yielded=<0/1> complete=<0/1> live=<n> protected=<n>` | none (observations, collector.cpp:243-248) | - | M (8a) |
| :1077 | DEBUG | `garbage collection reclaimed <n> local bytes` | none (trace callback, collector.cpp:242) | - | M (8a) |
| :1080 | TRACE | `maintenance: garbage collection yielded to foreground I/O` | Macha's loop | - | N/A until 8c |
| :1084 | TRACE | `maintenance: recent orphan deferred to exact grace deadline` | Macha's loop | - | N/A until 8c |
| :1088 | TRACE | `maintenance: garbage collection complete; waiting for an event` | Macha's loop | - | N/A until 8c |
| :1117 | TRACE | `maintenance: local rebalance yielded to foreground I/O` | none | - | M (local stages go to the node at step S, cutover :129-131) |
| :1121 | TRACE | `maintenance: local rebalance quiescent; waiting for an event` | none | - | M |
| :1140 | DEBUG | `maintenance: metadata head repair failed: <e>` | node.cpp:710 | DEBUG | S (8c) |
| :1149 | DEBUG | `maintenance: history truncation failed: <e>` | node.cpp:732 | DEBUG | S (8c) |
| :1177 | TRACE | `maintenance: scrub yielded to foreground I/O` | none | - | M |
| :1192 | DEBUG | `maintenance: scrub schedule persistence unavailable: <e>` | node.cpp:609 | DEBUG | R: `scrub schedule not kept: <e>` |
| :1195 | TRACE | `maintenance: scrub pass complete; next campaign scheduled` | none | - | M |
| :1223 | INFO | `storage held ledgers verified against the disk` | node.cpp:819 | INFO | S |
| :1240 | DEBUG | `maintenance: <message>` | Macha's loop | - | N/A |

## 4. Dagda-only lines at INFO or above

In store/, ledger/, node.cpp (and heads/ where the text has no Macha source):

| Dagda | Lvl | Text | Note |
|---|---|---|---|
| node.cpp:106 | INFO | `<step>`: `open message-types`, `open state-layout`, `open node-local-key`, `open storage-pool`, `open admission`, `open control-store`, `open cache`, `open claims`, `open reference-counts`, `open distributed-store`, `open heads`, `open availability`, `open repair`, `open storage-server`, `open maintenance` (:133-337); `stop maintenance`, `stop <part>`, `stop state-layout` (:374, :390, :414) | About 15 new INFO lines at each start and several at each stop, prefixed `dagda.node: `. The largest new source of journal text. |
| node.cpp:363 | INFO | `node open: domain <name> (<id>), <n> backends online, used <n> of <n>` | Replaces Macha's `node data storage ready` (section 3) |
| node.cpp:598 | INFO | `reloaded: <n> backends online, cache on\|off` | New on reload |
| node.cpp:95 | ERROR | `node refused: <why>` | Replaces Macha's two recovery ERRORs |
| node.cpp:218 | WARN | `data storage degraded: every configured backend is offline` | Replaces local_state.cpp:121 |
| ledger/retention.cpp:257 | INFO | `retention claims ledger created` | Replaces `retention claims moved into the ledger` |
| store/distributed_store.cpp:668 | WARN | `store job failed: <e>` | New |
| store/storage_pool.cpp:693 | WARN | `unreadable local record <id> on <path>: <e>` | New (sealed-record read, `get_sealed`) |
| store/storage_pool.cpp:746, :1095 | WARN | `unreadable local object <id> on <path> kept (key unchecked): <e>` | New; replaces `discarding/removing corrupt local object` until the key is checked |
| heads/exchange.cpp:109 | WARN | `metadata write copies not advertised: <e>` | No Macha source text found (search of `../macha/src/**`) |
| heads/reference_census.h:55 | WARN | `census did not match the tree; walking it again` | No Macha source text found |
| heads/reference_census.h:59 | WARN | `census cannot follow the tree; walking it again: <e>` | No Macha source text found |

Every other heads line at INFO or above (exchange.cpp, replica.cpp, space.cpp) has its text in
Macha's `src/metadata` (checked by substring search of `../macha/src/**/*.{cpp,hpp}`); those are
outside the files this audit covers and move at 8c. I did not compare their levels.

## 5. The golden sample (Macha 0.93.4, DEBUG, `../macha/build/macha-fixtures/goldens-0.93.4/`)

Store lines in `journal-a.log` (b is the same set, line numbers in brackets). "After" shows what
the journal reads once Dagda writes it.

| a [b] | Macha line (message part) | Source | Dagda after the port |
|---|---|---|---|
| 9, 17 [11, 17] | `storage accounting scan begin path=...` | local_store.cpp:2130 | S: `dagda.control: ...` (control store) / `dagda.pool.backend: ...` (data) |
| 10, 18 [10, 14] | `storage presence index warmed path=... objects=0 elapsed_ms=0` | local_store.cpp:2112 | S, same prefixes |
| 11 [8] | `storage backend probe begin path=.../data` | storage_pool.cpp:172 | S: `dagda.pool: ...` |
| 12, 19 [20, 21] | `storage accounting scan complete path=... used=0` | local_store.cpp:2239 | S, prefixed |
| 13, 21 [19, 23] | `storage held ledger seeded path=... objects=0 elapsed_ms=<n>` | local_store.cpp:606 | S, prefixed |
| 14 [13] | `storage backend online .../data elapsed_ms=<n> accounting=reconciling` | storage_pool.cpp:280 | S: `dagda.pool: ...` |
| 15 [15] | `data io pressure gate enabled expected_ms=25+120/MiB ...` | local_state.cpp:102 | M (only `dagda.node: open admission`) |
| 16 [16] | `node data storage ready used=0 capacity=1073741824` | local_state.cpp:117 | R: `dagda.node: node open: domain ..., 1 backends online, used 0 of ...` |
| 20 [22] | `retention claims moved into the ledger data=0 control=0` | storage/retention.cpp:398 | R: `dagda.claims: retention claims ledger created` |
| 22 [24] | `node metadata ready generation=1` | local_state.cpp:178 | Macha's until 8c; no Dagda counterpart |
| 28, 29 [-] | `metadata replica set formed active=1` | Macha src/metadata | Macha's until 8c; then heads/exchange.cpp:962, same text, `dagda.heads: ` |
| 31 [31] | `metadata availability changed state=writable previous=unavailable reason="local metadata state ready" generation=2 replicas=<n>/<n> required=1` | Macha src/metadata | Macha's until 8c; then heads/exchange.cpp:1407-1419, same format |
| 32, 39 [34] | `availability surveyed generation=2 referenced=0 local=0 ... prefixes_asked=0 table=walked\|kept method=diff` | availability_service.cpp:553 | R: `dagda.availability: availability surveyed generation=2 referenced=0 missing_here=0 unavailable=0 unknown=0 peers=<n> peers_failed=<n> rounds=0 method=diff` |
| 33, 34, 45 [35, 41, 42] | `garbage collection sweep node=aa2b32 skipped: inventory rebuilt this pass` / `skipped: quiet window` / `enabled` / `skipped: complete; waiting for an event` | maintenance.cpp:986 | Macha's until 8a; then R: `dagda.gc: garbage collection sweep skipped: quiet window` (no node=) |
| 36 [-] | `metadata repair deferred; continuing non-destructive maintenance: metadata accepted-head replication incomplete` | maintenance.cpp:514 | Macha's until 8c; then S (heads/exchange.cpp:1372) |
| 37 [32] | `availability: a peer could not finish a trie diff: peer does not answer trie_diff` | ledger/availability.cpp:101 | R: no line; the reason moves to ` why=` on the next |
| 38 [33] | `availability diff failed peer=<12 hex> questions=0 reply_bytes=0 waited_ms=0` | availability_service.cpp:484 | R: `dagda.availability: availability diff failed peer=<12 hex> questions=0 reply_bytes=0 why=<why>` |
| 53-55 [47-49] | `shutdown: service maintenance request_stop\|joining\|joined` | maintenance.cpp:129-133 | Macha's loop (N/A) |

Not store: allocator, HTTP, node/connectivity/RPC/dial-back, accounts, catalogue installed (a:30,
b:30), plugins and subsystems, `server local services ready`, and every other shutdown line.

New in the same run after the port (not in the golden): the `dagda.node: open ...` INFO steps and
`dagda.node: node open: ...` at start, and `dagda.node: stop ...` at stop (node.cpp:106, :363,
:374-414).

The golden run reaches no WARN or ERROR store line, so it confirms none of the trouble lines.

## 6. Lines operators grep for

- `control object unavailable` (the 0.93.x incident line): template SAME, WARN, now
  `dagda.control: control object unavailable id=<hex> peers_asked=<n> last_failure=<f>`. The
  `last_failure` wording changes (section 3) and the per-peer DEBUG `control object read <host>: <e>`
  is no longer written for calls that got no reply or a refusal (`store/control_objects.cpp:32-40`),
  so the DEBUG trail that preceded it in an incident is thinner. Note the scope `dagda.control` is
  shared with the control store's disk lines.
- `repair cannot source` and `repair found a local object it cannot read back`: see section 1.
  The second leaves the INFO journal entirely.
- `storage backend offline|online`, `storage durability barrier failed`,
  `storage durability reassertion failed`, `cannot read local object`, `discarding corrupt local object`,
  `removing corrupt local object`, `skipped unreadable pack region`, `truncated ...`,
  `DATA pack compaction deferred`, `DATA credit wait abandoned`, `DATA device pressure onset|released`,
  `storage held ledger corrected`, `storage held ledgers verified against the disk`: SAME, prefixed.
- `node data storage degraded`, `node data storage ready`, `node ... recovery failed`: REWORDED;
  a grep for `node data storage` finds nothing after the port.
- `object durability re-derived after incarnation change` (INFO) and
  `local storage durability barrier failed` (WARN): MISSING.

## 7. Unsure or not done

- Variable rendering of peer errors (Macha's RPC exception text against Dannan's `outcome_text` and
  `CallOutcome::failure`) was not compared word for word.
- Whether heads' `metadata retention barrier` line (heads/space.cpp:479) is meant to replace
  Macha's `DATA retention barrier` and `CONTROL retention claim` lines.
- Whether `repair position not saved` and `reason=retained_memory` are gone by ruling or are gaps.
- Scopes of the held ledger, sealed journal and object trie loggers were not traced end to end.
- Macha's lines in `src/metadata`, `src/catalogue` and the provider were not audited, beyond
  checking that the Dagda heads texts at INFO or above exist there.
- Whether Macha's facade (step S) or reachability provider (8a) will itself write the lines marked
  MISSING or N/A here (`data io pressure gate enabled`, `node data storage ready`,
  `retention release horizon namespace tree unavailable`): Macha's port is not written yet.

## 8. Restored (2026-11-13, for 0.1.2; the maintainer: "Restore everything")

Every REWORDED and MISSING line above now has Macha's text and level, except as listed here. Macha's
sink drops the `dagda.` scope at the store cutover (the Macha session, 2026-11-13), so the lines read
as Macha's did. Cases that failed first and pin the warn, error and info lines:
`Node.OpensAndStopsInOrderOnAFreshStateAndAReopenedOne` (the gate, data storage ready, claims moved, Dagda's
steps at DEBUG), `Node.AV18DiskIsRefusedNamingTheMigrationAndNothingChanges` and
`Node.ANodeWithoutCustodyIsRefused` (the two recovery ERRORs), `Node.NoBackendOnlineIsReadyButDegraded`,
`StoreDurabilityBarrier.AStaleProcessEpochIsRefused`, `...ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt`
(both re-derive INFOs and the quorum line), `...ALocalBarrierThatFailsIsWarnedOf`,
`StoreRepair.RepairPassesUnavailableAndCountsUnsourceable`, `StoreRepair.APushOfALocalObjectThatWillNotReadIsWarnedWithItsTotal`.
The debug and trace lines are restored by text without a case each.

Still different, each for a reason:
- `availability surveyed`: no ` table=`. The path table stays Macha's and is decided after this line;
  Macha's facade knows the word (`kept`, `followed`, `walked`).
- `opportunistic persistence skipped ... reason=retained_memory`: Dagda's local copies take no
  retained-memory lease, so the event does not happen. Adding the gate would change behaviour.
- `repair position not saved`: no repair cursor, by ruling ("repair diff only").
- `object durability re-derived after incarnation change`: one line per peer probed (`peers=1`), not
  one per barrier batch; Dagda's probes answer separately.
- `object durability quorum unavailable`: written when a synchronous wait on a deferred write ends short;
  each copy's `outcome="..."` is Dagda's evidence text, not Macha's outcome strings.
- Peer `error=` and `last_failure` texts come from Dannan's call outcomes. A refused CONTROL fetch's
  `last_failure` reads `<host>: error`, as Macha's message name for a refusal.
- `node data storage recovery failed` covers a DATA disk refused (v18 marker, lock held) or the pool
  failing to open; every other refusal is `node local state recovery failed`, Dagda's custody and layout
  refusals among them.
- `local storage durability barrier failed` is also written when this process's own ticket finds its
  placement changed (Macha warns of that too); another process's ticket stays quiet in both.
- `node metadata ready generation=` stays with Macha's replica until 8c.
- Dagda-only lines kept, since they report what Macha does not do: `unreadable local object ... kept (key
  unchecked)`, `unreadable local record`, `store job failed`, `reloaded:`, and heads' census and write
  copies warnings. Dagda's `open <step>`, `stop <step>` and `node open:` lines moved to DEBUG.
- At 8a the provider's own lines (`retention release horizon namespace tree unavailable`, its catalogue
  line) are Macha's to keep: Dagda logs only `retention release horizon unavailable: <e>` at DEBUG when the
  provider throws. A provider may now fill `Reachability::entries` and `extents` for the
  `DIAG maintenance-inventory` line, which prints 0 otherwise.

