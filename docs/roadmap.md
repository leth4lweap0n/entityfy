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
  value, EF Core `AsNoTracking`); measured 332 ns/row against 460 ns/row tracked
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
- `entityfy_migrate` CLI

**Repository**

- `entityfy` monorepo: `modules/` + `cmake/Dependencies.cmake` + `CMakePresets.json`
- A single SQLite dependency (sl3); the duplicated `third_party/sqlite` copy was removed
- Unified test registration under CTest (core + migration + context)
- `entityfy_bench`: SQLite comparative benchmark + PostgreSQL mode (libpq baseline)
- **Streaming row materialization**: `find`/`find_one`/`find_many`/`exists` materialize in place
  without an intermediate row vector (measured: no significant single-row gain, small gain for
  many-row reads - see `docs/benchmarks.md`)

## Planned

1. **Dirty-column update** — because core keeps no snapshot, `table_client::update` writes all
   columns; partial UPDATE exists only in the context layer.
2. **Compile-time no-tracking view** — `as_no_tracking()` returns `std::vector<T>` by value, so
   mutating a returned entity compiles and is silently ignored (the EF Core `AsNoTracking`
   footgun). A const-only wrapper would make that a compile error; it needs a view type that
   keeps the value movable, so it is a design decision, not a rename.
3. **Owned types and TPH inheritance** — not supported in the model layer. The owned/JSON need is
   in practice covered by `entityfy::value_converter<T>` (a composite type is written to a single
   column), but owned types **split across** columns (as in EF, e.g. `address_city`) and a TPH
   discriminator do not exist yet.
4. **Additional providers** — MySQL / SQL Server.
5. **Model scaffold from DB** — the `scaffold` command in the CLI generates a C++ model header
   **from the live schema** (the DB → model direction exists); the reverse direction (model → DB)
   is covered by the `snapshot` + `apply` commands, with no separate generation command.

## Known limitations

- The context module uses `entityfy::db_query_builder`; it is named this way to avoid a name
  collision with core's `entityfy::query_builder`.
- The migration module uses its own providers; there is no statement cache there.
- `save_changes_async()` is a thread offload, not I/O asynchrony (the drivers are synchronous).
- The `db_set<T>` members of a context must be declared by hand: C++26 reflection can inspect
  members but cannot synthesize them, so `ctx.users` cannot be generated from the entity type
  alone.
