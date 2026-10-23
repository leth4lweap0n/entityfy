# Benchmark notes

This file contains a **measured** and **honest** assessment of entityfy's query performance. The
harness is `modules/entityfy-demo/bench_compare.cpp` → `entityfy_bench`.

## Method

```bash
./build/release/bin/entityfy_bench 100000 5     # row count, repetition count
```

- Same process, same schema, same rows; **in-memory SQLite**, single thread.
- Three paths do the same work:
  1. **entityfy typed API** — including statement compilation, binding, and row materialization.
  2. **raw cached** — raw SQLite, the statement is compiled once and reused on every call.
  3. **raw prep-each** — raw SQLite, `prepare` + `finalize` on every call.
- Timing: not single-call time, but **total call time / call count**; the median of 5–7
  repetitions. (A per-call `clock()` measurement adds ~20–40 ns, so a micro-benchmark is
  misleading.)
- The query order is not shuffled (shuffled id/emails); every row is visited an equal number of
  times.
- Validation: row counts, sample row contents, and post-delete counters are compared
  before/after the measurement; a mismatch fails the benchmark.

### Unfair baselines were fixed

In the first version two comparisons were unfair and were fixed:

| Problem | Fix |
|---|---|
| `raw_range` read only `id` while entityfy materialized 20 full users | The baseline also reads `id + email + name` and copies the strings |
| Raw update wrote only `name` while entityfy wrote all non-PK columns | The baseline also writes `email + name` |
| All three delete variants deleted the same rows → the second variant measured a no-op | Each variant deletes from a separate row region; the deleted row count is verified |

Also, `find_many` returns **20 rows** per call (100 calls = 2.000 rows).

## Before the statement cache (100.000 rows)

| Operation | entityfy | raw cached | raw prep-each | /cached | /prep |
|---|---:|---:|---:|---:|---:|
| insert (transaction) | 468.505 rows/s | 1.386.897 rows/s | — | 2,96x | — |
| find (primary key) | 2,06 µs | 0,41 µs | 1,47 µs | 5,03x | 1,40x |
| find_one (unique email) | 2,93 µs | 0,90 µs | 2,05 µs | 3,26x | 1,43x |
| exists (unique email) | 1,93 µs | 0,44 µs | 1,35 µs | 4,39x | 1,43x |
| find_many (LIKE, LIMIT 20) | 12,75 µs | 6,56 µs | 8,83 µs | 1,94x | 1,44x |
| count | 9,73 µs | 8,33 µs | 9,44 µs | 1,17x | 1,03x |
| update (read + write) | 5,56 µs | 2,09 µs | 4,03 µs | 2,66x | 1,38x |
| remove_by_id | 2,20 µs | 1,31 µs | 1,93 µs | 1,68x | 1,14x |

### "Keys" (PK / unique) scaling

| Rows | PK entityfy | PK raw cached | unique entityfy | unique raw cached |
|---:|---:|---:|---:|---:|
| 1.000 | 1,92 µs | 0,30 µs | 2,37 µs | 0,49 µs |
| 20.000 | 1,95 µs | 0,34 µs | 2,59 µs | 0,67 µs |
| 100.000 | 2,06 µs | 0,41 µs | 2,93 µs | 0,90 µs |

The indexes work correctly: key access is nearly flat with row count.

## After the statement cache (100.000 rows)

Results from a clean build (`sl3 v1.2.53003`, Release):

| Operation | entityfy | raw cached | raw prep-each | /cached | /prep |
|---|---:|---:|---:|---:|---:|
| insert (transaction) | 788.067 rows/s | 1.406.765 rows/s | — | 1,79x | — |
| find (primary key) | 0,97 µs | 0,41 µs | 1,47 µs | 2,37x | **0,65x** |
| find_one (unique email) | 1,57 µs | 0,89 µs | 2,09 µs | 1,76x | **0,75x** |
| exists (unique email) | 0,94 µs | 0,44 µs | 1,33 µs | 2,17x | **0,71x** |
| find_many (LIKE, LIMIT 20) | 12,31 µs | 6,67 µs | 8,94 µs | 1,85x | 1,38x |
| count | 8,86 µs | 8,58 µs | 9,33 µs | 1,03x | 0,95x |
| update (read + write) | 3,49 µs | 2,12 µs | 4,04 µs | 1,65x | 0,86x |
| remove_by_id | 1,67 µs | 1,30 µs | 1,93 µs | 1,29x | 0,86x |

(Under high machine load the same workload measured in the 1,7–3,1 µs band; the **ratios** are
more stable.)

With the statement cache, entityfy became faster even than raw SQLite that **prepares on every
call** (0,65–0,86x on point queries). The remaining gap (1,3–2,4x versus `raw cached`) comes from
`std::string` copies during materialization and from row-vector allocations.

> Run-to-run variation: ±%5 on point queries, and deviations up to ±%30 on write paths such as
> `update` and `remove_by_id` (b-tree rebalancing cost). Rather than deciding from a single run,
> compare medians.

### Statement cache effect — same process, same data (reliable method)

Absolute times can vary **by up to a factor of two** with machine load (in the same session the
raw SQLite baseline measured in the 0,41 µs → 0,97 µs range). Therefore the cache's effect is
measured with an **on/off** comparison **in the same process** (50.000 rows, 5 repetitions):

| Operation | cache on | cache off | Gain |
|---|---:|---:|---:|
| find (primary key) | 1.968 ns | 4.534 ns | **2,30x** |
| find_one (unique email) | 3.050 ns | 5.807 ns | **1,90x** |

Because this measurement was made against the same DB, the same data, in the same process, it is
independent of machine load and is the authoritative answer to "what does the cache gain". The
"before/after" tables were taken in different sessions, so the **ratios** apply (ratios are less
affected by load), while absolute values are context only.

## Multi-row INSERT (measured after the statement cache)

For a model with explicit keys, `insert_many` produces a single statement; compared with the
row-by-row path (SQLite `:memory:`, 20.000 rows):

| Path | Throughput |
|---|---:|
| `entityfy insert` (row by row) | 351k rows/s |
| `entityfy insert_many` (single statement) | **1,40M rows/s** |
| Gain | **3,98x** |

`insert_many` is split according to the backend's batch parameter limit (SQLite 900, PostgreSQL
1024 parameters); for models with auto-increment keys, generated ids are fetched row by row, so
that path runs row by row.

## PostgreSQL measurement (same machine, local server)

When `ENTITYFY_PG_URI` is provided, `entityfy_bench` also measures the PostgreSQL path: entityfy,
raw libpq `PQprepare`+`PQexecPrepared`, and raw `PQexecParams` paths are compared (5.000 rows):

| Operation | entityfy | raw prepared | raw params | E/prep | E/params |
|---|---:|---:|---:|---:|---:|
| find (primary key) | 83,3 µs | 53,9 µs | 99,9 µs | 1,55x | 0,83x |
| find_one (unique email) | 71,1 µs | 56,2 µs | 99,9 µs | 1,26x | 0,71x |
| insert_many | 110k rows/s | — | — | — | — |

**Statement cache:** the PostgreSQL backend now prepares statements too (`PREPARE` through
taocpp::pq, LRU of 32 names by default, deallocated on eviction and after DDL). Measured in the
same process with the cache off and on:

| Operation | cache off | cache on | gain |
|---|---:|---:|---:|
| find (primary key) | 129,0 µs | 83,3 µs | 1,55x |
| find_one (unique email) | 141,7 µs | 71,1 µs | 1,99x |

**Commentary:** in PostgreSQL the absolute time is determined by network latency (~60–90 µs). With
the server side cache the ORM layer adds 1,3–1,6 times the raw driver instead of 1,3–2,3 times;
against `PQexecParams` (prepare on every call) entityfy is now faster (0,7–0,8x). What remains is
the per-call round trip, not parsing or planning.

## Context layer (change tracking)

`entityfy_bench` also measures the context layer, i.e. the `db_set` / `db_context` path that the
core numbers above do not cover. Inserts, lookups and updates run on an in-memory database so
they are comparable with the core table. The cold lookup and the bulk load use a file backed
database and a **fresh context that tracks nothing**, so both really run `SELECT` + materialize +
attach instead of hitting the identity map.

| Operation | Measured | Notes |
|---|---|---|
| `add` + `save_changes` (20.000 rows, one transaction) | 606.000 rows/s | typing + tracking + generated key write-back |
| `find` (identity map hit) | 14 ns | hash lookup, no SQL |
| `find` (database hit, cold) | 32 µs | SELECT + materialization + attach |
| `to_list` (5.000 rows into a fresh context) | 460 ns/row | real load + attach; the bench prints the measured ratio (71x cheaper per row than a cold `find`, 63-71x across runs) |
| `to_list` (same rows, `as_no_tracking()`) | 332 ns/row | materialize only: no snapshot, no identity map entry - tracking costs **1,39-1,52x** per loaded row |
| `save_changes` with 1 dirty row | grows with the tracked set | 244 µs @2k, 973 µs @8k, 2,62-2,88 ms @20k |
| `save_changes` **per tracked entity** | 141 ns | the snapshot scan, divided by the tracked set (20k); 141-144 ns across runs |

Every number is a median over `repetitions`; the cold lookup and both bulk loads open a **fresh
context per repetition**, so the ratios compare measured medians rather than single runs.
Repeating the whole run moves the absolute values with machine load. Five `20000 5` runs gave
576.000-606.000 rows/s for the insert, 457-482 ns/row for the tracked bulk load, 316-335 ns/row
without tracking (the last three runs, the only ones that measured it) and 2,62-2,88 ms for the
one-row save - 141-144 ns per tracked entity. The printed ratios stayed between 63x and 71x
(cold find / tracked load) and 1,39-1,52x (tracked / no-tracking).

A one-row update is O(tracked entities) for two independent reasons, and only one of them
remains:

1. **Snapshot comparison (remains).** `save_changes()` has to decide which entities changed, and
   without write hooks the only way is comparing every tracked entity against its snapshot. Every
   unchanged entity costs a field-by-field comparison (141-144 ns each on this model). This is the
   same work EF Core does in `ChangeTracker.DetectChanges()` unless proxies are used.
2. **Pre-save backup (removed).** `begin_save()` used to deep-copy the whole tracked set so that a
   failed save could restore it. That was a second, larger O(n) term with two string copies per
   entity. It is now an undo log: `mark_undo(index)` stores the pre-save image only for items the
   save actually touches, so an update of one row records one entry. The same change also moved
   the column list to compile time (`update_members<T>()`) and defers the `UPDATE` string and bind
   vector until a field really differs; together they cut the 20k measurement from 5,37 ms to
   2,62 ms.

The columns of the modified row are still the only ones written - the scan is CPU work, not extra
SQL. Removing the remaining term needs write tracking (explicit `mark_modified()` or proxy
setters), which is on the roadmap.

## Honest assessment

1. **The statement cache was the biggest gain** (same-process on/off measurement: PK 2,26x,
   unique 1,88x) and is on by default (capacity 32).
2. The "good-looking" 2,5 µs numbers before the cache were misleading because they were
   **without a baseline**; the real gap against a raw prepared statement was 3–6x.
3. **Multi-row INSERT landed in core**: `insert_many` became 4,1x faster with a single statement;
   the remaining gap is now only row-materialization allocations (`vector<vector<sql_value>>`)
   and dirty-column optimization for `table.update`.
4. `count` and `find_many` benefit little from the cache because they are scan-heavy; this is
   expected behavior.
5. **On PostgreSQL the ORM is not the bottleneck**: entityfy is 1,3–2,1 times the raw driver;
   network latency determines the absolute time. The PostgreSQL backend does not use a statement
   cache because taocpp::pq does not expose `PQexecPrepared` (see the roadmap) — the raw
   `PQexecParams` path being faster than entityfy follows from the same fact.
6. There is **no** dirty-column (partial) update in core: core keeps no snapshot, and this is the
   context layer's responsibility. `table_client::update` writes all columns.

## Limitations

- SQLite measurements are on `:memory:`; disk I/O and concurrency were not measured.
- Absolute times depend on machine load (deviation up to 2x in the same session); the **ratios**,
  which are less load-sensitive, and **on/off comparisons** made in the same process should be
  taken as the basis.
- The PostgreSQL measurement is against a local server; it does not represent a remote
  server/global network latency.
- Single thread, single connection.
- p95/p99 are not reported; the batch-call median is used.
- The raw baseline reads a minimal structure; entityfy produces a typed object + string copy (the
  difference is deliberate).
- `raw cached` is the best case; `raw prep-each` is closer to entityfy's pre-cache behavior.

## Reproduction

```bash
cmake --preset release && cmake --build --preset release
./build/release/bin/entityfy_bench 100000 5

# PostgreSQL path (the database must exist)
ENTITYFY_PG_URI=postgresql://postgres:postgres@127.0.0.1:5432/entityfy_bench \
  ./build/release/bin/entityfy_bench 5000 3
```

The benchmark creates its tables in the given database and drops them again when the PostgreSQL
section finishes. Still point `ENTITYFY_PG_URI` at a dedicated database (`entityfy_bench` above):
a run that is interrupted mid-way can leave `bench_users*` behind, and the migration test would
then report a schema that no longer matches its snapshot.

Environment: Windows 10 x64, GCC 16.2, Release, SQLite 3.53.3, PostgreSQL 18.6.
