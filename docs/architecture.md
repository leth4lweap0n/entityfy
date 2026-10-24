# Architecture

## Layers

```
                 ┌──────────────────────────────────────────┐
                 │ modules/entityfy-demo                    │
                 │ console · bench · migration probe        │
                 └───────────────┬──────────────────────────┘
                                 │
      ┌──────────────────────────┼──────────────────────────┐
      │                          │                          │
┌─────▼─────────────┐  ┌─────────▼──────────┐  ┌────────────▼─────────┐
│ entityfy-context  │  │  entityfy-migrate  │  │   entityfy-core     │
│ change tracking   │  │  snapshot/diff     │  │ model · SQL · query │
│ identity map      │  │  apply/rollback    │  │ backends · cache    │
└─────┬─────────────┘  └─────────┬──────────┘  └────────────┬─────────┘
      └──────────────────────────┼──────────────────────────┘
                                 │
                    entityfy::client / entityfy::detail::backend
                                 │
                 ┌───────────────┴───────────────┐
                 │ SQLite (sl3)      PostgreSQL   │
                 │                   (libpq)       │
                 └───────────────────────────────┘
```

- **core** is the single source of truth: annotation → model → SQL. The other modules depend on
  core, and never the reverse.
- **context** adds change tracking, transactions, and eager loading on top of core; it generates
  no SQL itself.
- **migrate** uses core's model/schema generation, takes snapshots through its own providers
  (SQLite/PostgreSQL), and writes migration files.
- **demo** only produces the binaries; it has no dependency on library code.

## Namespace decisions

The entire library is consolidated under a single root namespace: `entityfy`.

- The name `entityfy` was chosen because `reflect` is a **keyword** in C++26 (a reflection
  language feature), so it would easily collide with other class/namespace names.
- `entityfy::migrate` holds the types belonging to the migration module; it does not collide with
  core.
- The context module exposes the `entityfy::db_context` class and the `entityfy::db_set<T>`
  type. To avoid a name collision with core's `entityfy::query_builder<Model>`, the context
  query builder is named `entityfy::db_query_builder<Model>`.

## Dependency management

Third-party dependencies are resolved at the top level in `cmake/Dependencies.cmake`:

| Dependency | Usage |
|---|---|
| `sl3` | The SQLite backend and the migration provider use the **same** SQLite copy |
| `libpq` (`PostgreSQL::PostgreSQL`) | PostgreSQL backend; system package found by CMake |
| `nlohmann_json` | Migration snapshot/script JSON |
| `Catch2` | Core tests |
| `spdlog` | Core examples |

The separate `third_party/sqlite` copy in the migration module was removed earlier; two SQLite
copies in the same process carried a risk of symbol collisions.

## Statement cache

`entityfy::client` used to recompile the statement on every call; this produced a 3–6x gap
against raw SQLite on point queries. The SQLite backend now keeps compiled statements in an LRU:

- Key: the SQL text. Value: `sl3::Command` (the compiled statement).
- Capacity: `client::set_statement_cache_capacity(n)`, default 32, `0` = disabled.
- Eviction: the least recently used entry is dropped; `clear_statement_cache()` clears them all.
- Parameters are rebound with `resetParameters` on every execution; bind values do not leak from
  one call to the next (the test verifying this is in `StatementCache.test.cpp`).
- Backend concurrency: the statement cache and the connection are protected by
  `std::recursive_mutex`.

Measured effect: [docs/benchmarks.md](benchmarks.md).

## Test strategy

- Core: Catch2, an independent `TEST_CASE` per module; registered automatically with CTest via
  `catch_discover_tests`.
- Migration and context: their own small harnesses (`require(...)`); the PostgreSQL test is
  enabled with `ENTITYFY_PG_URI`.
- If PostgreSQL is not required the test is skipped; with `ENTITYFY_REQUIRE_PG=1` a skip counts
  as a failure.

## Deliberate limitations

- The migration module uses its own providers; there is no statement cache there.
- There is no async/coroutine API; `db_context_pool` provides connection and context reuse.
