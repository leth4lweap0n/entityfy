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

The remaining work is large and none of it is started; each entry records the
surface that has to change, so a session can pick one up without redoing the
analysis. Everything above this section is implemented and covered by the test
suite.

1. **Statement caching for MySQL and SQL Server** — the clients prepare each statement again; a
   bounded cache like the PostgreSQL one would cut the parse round trip.

## Smaller follow-ups

Measured or diagnosed, not started:

- **PostgreSQL pipeline mode / batching** — with the statement cache in place the per-call round
  trip is the dominant cost (`docs/benchmarks.md`); taocpp supports pipelines.
- **Statement cache in the migration providers** — they prepare on every statement (known
  limitation).
- **`as_no_tracking()` with eager loading** — `include` would have to return read-only pairs.
- **Deduplicate the context read path** — `include`/`join` still collect `vector<row>`; `find`,
  `to_list`, `from_sql` and `as_no_tracking` already stream through `client::query_each`.

## Known limitations

- The context module uses `entityfy::db_query_builder`; it is named this way to avoid a name
  collision with core's `entityfy::query_builder`.
- The migration module uses its own providers; there is no statement cache there.
- `save_changes_async()` is a thread offload, not I/O asynchrony (the drivers are synchronous).
- The `db_set<T>` members of a context must be declared by hand: C++26 reflection can inspect
  members but cannot synthesize them, so `ctx.users` cannot be generated from the entity type
  alone.
