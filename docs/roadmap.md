# Roadmap

## Completed

**Core**

- Model/annotation system, DDL generation, schema validation, and drift report
- query DSL: filters, ordering, paging, projection, aggregates, grouped aggregates
- SQLite and PostgreSQL backends
- **Prepared statement cache** on both backends (LRU, default capacity 32, bind-leak tested,
  SQLite `sqlite3_stmt` + PostgreSQL named `PREPARE` plans; measured 1,55x / 1,99x against the
  cache being off)
- **Multi-row INSERT** (`insert_many`): a single statement for models with explicit keys, split
  according to the batch limit; measured 4,1x gain
- **SQL logger** (`client::set_logger`) and batch parameter limit
- Error classification (`unique_violation`, `foreign_key_violation`, ...)

**Context**

- Identity map, snapshot-based change tracking, partial UPDATE
- **Manual change tracking**: `set_auto_detect(false)` + `mark_modified(entity)`; a save visits only
  the changed entities (measured 5,5 µs at 50.000 tracked rows against 10,6 ms with auto detection)
- **Optimistic concurrency parity (P0/P1)**: structured `concurrency_error` with the conflicting
  entries (token, current database row), `database_values` / `accept_changes` (client wins and
  per-member merge) / `attach(value, original)`, `[[= entityfy::concurrency_token]]` on any integral
  member, the token checked by deletes, and cancelling a pending insert no longer issues a DELETE;
  covered on SQLite and PostgreSQL
- **No-tracking reads**: `query().select(...)` (tuples) and `as_no_tracking()` (full entities by
  value, EF Core `AsNoTracking`); measured 332 ns/row against 460 ns/row tracked.
  `to_list()` returns a read-only result, so mutating a row is a compile error
- **No-tracking eager loading**: `as_no_tracking().include<Child>()` returns owning
  `(parent, std::vector<child>)` pairs grouped by the reflection-derived foreign key; value
  navigations are filled, pointer navigations stay empty because a pointer into the result would
  dangle
- **Streamed context reads**: `include`, `include_parent`, both joins and the tracked query
  builder consume rows through `client::query_each` instead of buffering `vector<row>`; measured
  neutral on throughput (0,91-0,96x) and constant memory in `docs/benchmarks.md`
- FK dependency order: INSERT parent-first, DELETE child-first (`ON DELETE RESTRICT` supported)
- Nested transactions/savepoints, retry policy, batch splitting
- EF Core style surface: `db_context` / `db_set<T>` with `save_changes()`, `add()`, `find()`,
  `query()`, `include()`, `include_parent()`
- Reflection driven model registry: `Model()` metadata and `migrate_all()` table creation in FK order
- 1:N / N:1 eager loading with the foreign key read from the `references` annotation, two- and
  three-table joins, navigation fixup
- Bulk `remove_all()` / `update_all()`, JSON column
- `db_context_pool` (lease-based context reuse)
- **Asynchronous saving**: `save_changes_async()` (worker thread; saves are serialized by a
  context-local mutex — not I/O asynchrony, a thread offload)

**Migration**

- Snapshot, diff, apply, rollback, baseline, checksum protection
- Runnable SQL script generation (`script --all`), generation from an empty source
- C++ model header generation from the live schema (scaffold)
- `entityfy_migrate` CLI (scaffold verified end to end: the generated header compiles with
  `-fsyntax-only`)

**Repository**

- `entityfy` monorepo: `modules/` + `cmake/Dependencies.cmake` + `CMakePresets.json`
- A single SQLite dependency (sl3); the duplicated `third_party/sqlite` copy was removed
- Unified test registration under CTest (core + migration + context)
- `entityfy_bench`: SQLite comparative benchmark + PostgreSQL mode (libpq baseline)
- **Owned value objects**: `[[= entityfy::owned]]` splits a struct across prefixed columns,
  through `describe_model`, DDL, INSERT/UPDATE/UPSERT, `insert_many`, the row mapper and
  migrations; nested owned values and inner annotations supported
- **SQL Server support**: opt-in ODBC backend (`sqlserver://`, T-SQL dialect with
  `IDENTITY(1,1)`, `OUTPUT INSERTED`, `MERGE` upserts, `OFFSET/FETCH`), context integration and a
  migrate provider (identity columns, dependent index rebuilds, named default constraints); SQL
  Server has no `CREATE INDEX IF NOT EXISTS`, has no `RESTRICT` keyword, and `AVG` over integers is
  cast to FLOAT
- **PostgreSQL pipeline batching**: `insert_many` chunks go through one libpq pipeline
  (`execute_batch`, one round trip instead of one per chunk); measured 1,05-1,08x on localhost,
  larger as latency grows
- **Typed transient errors**: `serialization_failure` (40001), `deadlock_detected` (40P01) and
  `database_busy` (SQLite busy/locked) derive from `transient_error`; the context retry policy
  checks the type before falling back to message matching
- **Migration-provider statement cache: decided against by measurement** - each migration
  statement is unique (an LRU cannot hit); prepare costs 10,5 us (SQLite DDL) / 81 us (PG round
  trip) once per statement. Numbers and rationale in `docs/benchmarks.md`
- **PostgreSQL `xmin` token**: `[[= entityfy::xmin]]` maps a member onto the `xmin` system column;
  DDL/inserts/updates skip it, selects and the concurrency `WHERE` read it, and the value is read
  back after writes. Covered on PostgreSQL by an out-of-band update conflict without an
  application token column
- **Statement cache for MySQL and SQL Server**: `MYSQL_STMT` handles and ODBC statements stay in
  an LRU (default capacity 32), resetting the result set/parameter bindings between uses; measured
  by the backend cache tests (size, capacity changes, results after disabling)
- **MySQL/MariaDB support**: opt-in core backend (`mysql://`, `mysql` dialect, prepared statement
  path, `information_schema` introspection, error classification), context integration and a
  migrate provider with its own DDL/diff path and tests; MySQL has no transactional DDL
- **TPH inheritance**: `[[= entityfy::type_tag{...}]]` derived models share the base table and
  inherit its columns, `migrate_tph<Base, Types...>()` creates the union table (subtype columns
  nullable), CRUD is discriminator-scoped and `find_all_types()` loads the hierarchy as variants;
  single base class, no tracked/eager-loaded polymorphic reads
- **Partial UPDATE in core**: `update_partial(entity, &T::a, ...)` writes only the named columns
- **Streaming row materialization**: `find`/`find_one`/`find_many`/`exists` materialize in place
  without an intermediate row vector (measured: no significant single-row gain, small gain for
  many-row reads - see `docs/benchmarks.md`)
- **Composite and non-integral keys (P0)**: `entityfy::row_key` key lists, table-level and static
  primary keys including string and UUID, key-based statements, context identity and concurrency,
  and the migration diff for table-level keys
- **Lazy loading (P1)**: `entityfy::lazy<T>` reference and collection handles bound to tracked
  entities, typed lifecycle errors, one-query loads through the identity map, eager-loading cache
  fill, and reset/rebind after writes and rollback
- **Task primitives (P2.1)**: `entityfy::task<T>`/`task<void>`, cancellation hooks, `then`
  flattening, `inline_executor` and a bounded `thread_pool_executor`
- **Native async PostgreSQL (P2.2)**: raw libpq throughout; one in-flight operation per
  connection, a per-backend reactor using `PQsocketPoll`, nonblocking execution/query/pipeline
  batches, and cancellation through the libpq 17+ cancel API (SQLSTATE 57014 → `task_cancelled`)
- **Async client/table surface (P2.3)**: `async_mode`, `execute_async`, `query_async`,
  `execute_batch_async`, `query_each_async`, and async `table_client` CRUD built from the same
  statement builders
- **SQLite async facade (P2.4)**: reports `thread_offload`, one lazily created bounded worker per
  connection, FIFO, cooperative cancellation before a job starts
- **Async context save (P2.5)**: `save_changes_async()` returns `entityfy::task<save_result>`, a
  single-worker executor runs the save/retries, and a save gate rejects overlapping saves;
  destroying a context drains an in-flight save

## Next

EF Core parity on SQLite and PostgreSQL. The slice plan, contracts and acceptance criteria live in
[parity-plan.md](parity-plan.md); each slice is committed only when its tests and the full suite
are green.

1. **P0 composite and non-integral keys** — complete (row_key, key lists, table-level and
   string/UUID keys, key-based statements, identity/concurrency, migration diff). Altering the
   columns of an existing key (P0.3b2) remains.
2. **P1 lazy loading** — complete: P1.1-P1.4 (handles, binding, loaders, include cache,
   lifetimes) and P1.5 (composite foreign keys in include/lazy navigation).
3. **P2 async I/O** — complete for the implemented slices: task/executors, native nonblocking
   PostgreSQL, client/table async, SQLite thread-offload facade, task-based context save with a
   save gate. Remaining: `transaction_async`, native async `saveable` methods and
   `db_context_pool::acquire_async`.
4. **P3 ODBC** — decided, no code: SQLite and PostgreSQL keep their native drivers; the frozen SQL
   Server backend's gaps stay documented in the parity plan.

### Slice status

| Slice | Scope | Status |
|---|---|---|
| P0.0 | `row_key`, canonical ordering | complete |
| P0.1 | composite/string keys in models, DDL, statements | complete |
| P0.2 | key-based identity map and concurrency entries | complete |
| P0.3 | migration diff for new keys, key add/drop, introspection | complete; altering an existing key (P0.3b2) open |
| P0.4 | key tests and docs | complete |
| P1.1 | `lazy<T>` handles, cache, typed errors | complete |
| P1.2 | context binding, lifetimes, write/rollback invalidation | complete |
| P1.3 | loaders and include cache fill | complete |
| P1.4 | lazy tests and docs | complete |
| P1.5 | composite foreign keys in relations/navigation | complete |
| P2.1 | `task<T>`, executors | complete |
| P2.2 | native nonblocking PostgreSQL | complete; `transaction_async` open |
| P2.3 | async client/table surface | complete |
| P2.4 | SQLite thread-offload facade | complete |
| P2.5 | task-based context save, save gate | complete; native async `saveable` and `acquire_async` open |
| P2.6 | async tests and docs | complete for the shipped slices |
| P3 | ODBC decision | decided: no new ODBC work |

The other backends stay buildable and tested but get no new features for now (see
[Deferred](#deferred-other-providers)).

## Deferred: other providers

Paused deliberately; the code stays compiled and tested, no feature work until this section is
picked up again.

- **MySQL/MariaDB**: no transactional DDL (inherent), SQL-dump backups only (no `mysqldump`),
  `[[= entityfy::json]]` is `LONGTEXT` because MariaDB reports its alias that way.
- **SQL Server / ODBC**: the migrate provider refuses primary-key/identity changes and rebuilds
  dependent indexes around `ALTER COLUMN`; backups are SQL dumps (no `BACKUP DATABASE`).
- **Concurrency parity P0/P1 above is provider-agnostic** (context/core), so it also applies to
  these backends; the `xmin` system-column token is PostgreSQL-only.

## Known limitations

- The context module uses `entityfy::db_query_builder`; it is named this way to avoid a name
  collision with core's `entityfy::query_builder`.
- The migration module uses its own providers and runs every statement once; the statement-cache
  decision was made against implementation by measurement (`docs/benchmarks.md`).
- `save_changes_async()` is a thread offload, not I/O asynchrony (the drivers are synchronous).
- Migrations add and drop key columns, but altering the columns of an existing primary key in
  place (P0.3b2) is not implemented. Composite foreign keys are created and navigated; the migrate
  providers still describe them one column at a time.
- The `db_set<T>` members of a context must be declared by hand: C++26 reflection can inspect
  members but cannot synthesize them, so `ctx.users` cannot be generated from the entity type
  alone.
