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
- **No-tracking reads**: `query().select(...)` (tuples) and `as_no_tracking()` (full entities by
  value, EF Core `AsNoTracking`); measured 332 ns/row against 460 ns/row tracked.
  `to_list()` returns a read-only result, so mutating a row is a compile error
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

## Next

The focus is SQLite and PostgreSQL. The other backends stay buildable and tested but get no new
features for now (see [Deferred](#deferred-other-providers)). Each entry records the surface that
has to change so a session can pick it up without redoing the analysis. Everything above this
section is implemented and covered by the test suite.

### Change tracker: EF Core concurrency parity

1. **Structured concurrency error (P0)** — an update/delete that affects zero rows currently throws
   a plain `std::runtime_error` with the key in the message. Surface:
   `modules/entityfy-context/include/entityfy/db_set.hpp` (save/delete paths),
   a new `concurrency_error` with `entries()` carrying table, key, original values and the current
   database row. Acceptance: a stale save throws `concurrency_error` whose entry exposes the
   winner's values; existing suites keep passing.
2. **Resolution APIs (P0)** — no way to resolve a conflict without losing work: `refresh()`
   overwrites the entity and resets it to unchanged. Surface: `db_set<T>` plus
   `docs/context.md`. Add `database_values(entity)` (read without overwriting),
   `accept_changes(entity, database_row)` (client wins: original snapshot := DB row) and a
   per-member overload for merges. Acceptance: client-wins and merge scenarios save successfully
   and are covered by the context suite.
3. **Delete token check (P0)** — deletes use `DELETE ... WHERE id = ?` only, so a stale delete wins.
   Surface: `entityfy::delete_by_id_statement` (core) plus the context delete path. Acceptance:
   a stale delete throws `concurrency_error`; a delete of an already removed row reports it.
4. **Token selection and attach (P1)** — the token member must be named `version`; disconnected
   entities cannot carry a separate original snapshot. Surface: annotations, `db_set<T>`.
   Add `[[= entityfy::concurrency_token]]` for any integral member and
   `attach(value, original)`. Acceptance: a model whose token is named `revision` conflicts
   correctly, and a stale attached entity conflicts.
5. **PostgreSQL `xmin` token (P2)** — the store-generated token EF Core users get from Npgsql.
   Surface: `postgresql_backend.cpp` select/update paths, model mapping. Acceptance: a model using
   the system column conflicts after an out-of-band update without an application column.

### SQLite and PostgreSQL

6. **`as_no_tracking()` with eager loading** — a no-tracking query currently rejects `include`;
   the include path in `db_set` would have to carry read-only pairs and merge them by key.
   Surface: `modules/entityfy-context/include/entityfy/db_set.hpp`. Acceptance: an eager-loaded
   no-tracking read returns the same rows as the tracked variant, with a scenario.
7. **Deduplicate the context read path** — `include`/`join` still collect `vector<row>` before
   materializing. Surface: the same file. Acceptance: rows are built in the consumer, all suites
   green, delta recorded in `docs/benchmarks.md`.
8. **PostgreSQL pipeline mode / batching** — with the statement cache in place the per-call round
   trip dominates. Surface: `postgresql_backend.cpp` batch path. Acceptance: batched writes use a
   pipeline, the benchmark table records before/after, the PostgreSQL suite stays green.
9. **Type-based transient errors and retry** — retries match message text ("deadlock detected",
   "database is locked"). Surface: `postgresql_backend.cpp`, `sqlite_backend.cpp`, `error.hpp`,
   `db_context` retry predicate. Acceptance: PostgreSQL `40001`/`40P01` and SQLite
   `SQLITE_BUSY`/`SQLITE_LOCKED` map to dedicated error types and the retry policy keys off them.
10. **Statement cache in the migration providers** — SQLite/PostgreSQL only for now; decide by
    measurement. Acceptance: the measurement lands in `docs/benchmarks.md` and the outcome
    (implementation or documented decision) is recorded.

## Deferred: other providers

Paused deliberately; the code stays compiled and tested, no feature work until this section is
picked up again.

- **MySQL/MariaDB**: no transactional DDL (inherent), SQL-dump backups only (no `mysqldump`),
  `[[= entityfy::json]]` is `LONGTEXT` because MariaDB reports its alias that way.
- **SQL Server / ODBC**: the migrate provider refuses primary-key/identity changes and rebuilds
  dependent indexes around `ALTER COLUMN`; backups are SQL dumps (no `BACKUP DATABASE`).
- **Concurrency parity P0/P1 above is provider-agnostic** (context/core), so it also applies to
  these backends; P2 (`xmin`) is PostgreSQL-only by design.

## Known limitations

- The context module uses `entityfy::db_query_builder`; it is named this way to avoid a name
  collision with core's `entityfy::query_builder`.
- The migration module uses its own providers; there is no statement cache there (tracked as a
  measured decision for SQLite/PostgreSQL).
- Concurrency: until the change tracker items land, a conflict without a token column is a plain
  error, deletes are not versioned, and resolving a conflict means reloading and re-applying edits.
- `save_changes_async()` is a thread offload, not I/O asynchrony (the drivers are synchronous).
- The `db_set<T>` members of a context must be declared by hand: C++26 reflection can inspect
  members but cannot synthesize them, so `ctx.users` cannot be generated from the entity type
  alone.
