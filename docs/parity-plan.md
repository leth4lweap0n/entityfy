# EF Core parity plan (SQLite and PostgreSQL)

The goal is EF Core's behaviour, not its C++-impossible surface. The column DSL stays
member-pointer based; there is no expression-tree LINQ. MySQL/MariaDB and SQL Server remain
compiled and tested when enabled but get no feature work in this plan (see the Deferred section
in [roadmap.md](roadmap.md)).

## Decisions

- **Keys** become a general `entityfy::row_key` (ordered SQL values). Single-column integral keys
  remain the fast path; string, GUID-as-string and composite keys use the same abstraction.
  `AUTOINCREMENT`/`IDENTITY` stays valid only for a single integral key column.
- **Lazy loading** uses an explicit `entityfy::lazy<T>` handle, the C++ equivalent of a tracking
  proxy without runtime proxies: the handle is a normal member marked `[[= entityfy::ignore]]`,
  is bound when an entity is tracked, and loads on `get()`/`operator*`. No-tracking entities and
  handles whose context is gone fail with a typed `lazy_error` instead of silently returning
  empty data.
- **Async I/O** is native for PostgreSQL (raw libpq nonblocking I/O driven by a coroutine
  executor) and an honest thread-offload facade for SQLite, which has no nonblocking C API. The
  capability is reported by `async_capability` (`native` / `thread_offload`) instead of pretending
  both are equal.
- **ODBC**: no new ODBC work for SQLite or PostgreSQL. Both already use their native drivers
  (sl3/libsqlite3 and libpq) and the native paths expose features ODBC would flatten (pipeline,
  named plans, SQLSTATE, `xmin`, PRAGMA). ODBC stays an implementation detail of the frozen SQL
  Server backend. Its known gaps are recorded at the end of this document instead of being fixed
  opportunistically.

## Phase 0 — composite and non-integral primary keys

Slices are sequential; each one keeps the current single-integral-key behaviour green.

### P0.0 `row_key` core

**Status:** implemented (`key.hpp`, canonical ordering; tests in the core suite).

New `entityfy-core/include/entityfy/key.hpp`:

```cpp
class row_key
{
public:
    row_key() = default;                       // empty key
    explicit row_key(std::vector<sql_value> parts);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const sql_value& part(std::size_t index) const;
    [[nodiscard]] std::span<const sql_value> parts() const noexcept;
    [[nodiscard]] std::string to_string() const;   // diagnostics only
    // equality, ordering and hash compare canonical parts element-wise
};

struct row_key_hash;
```

`canonical_sql_value` gives a total order over `sql_value` without lossy text conversion
(numeric values by value, strings/bytes by content and length). The identity map and concurrency
entries use `row_key`; diagnostics use `to_string()`.

Acceptance: unit tests for equality/order/hash, empty keys, mixed numeric/string parts, and
`("a","bc") != ("ab","c")`.

### P0.1 Models, DDL and statements

**Status:** implemented for SQLite, PostgreSQL and the frozen backends (create/read/update/delete/upsert by composite and string keys).

- `model_descriptor` gains `primary_key_indices`; `primary_key_columns(model)` returns the key
  columns in declaration order. `primary_key_column(model)` stays for the single-key fast path and
  reports a clear error when the model has a composite key.
- `describe`/`validate_model` accept any number of `primary_key` annotations, keep the implicit
  `id` fallback only when none is annotated, reject optional and owned key parts, and reject
  `auto_increment` on a composite key at compile time.
- DDL: a single key keeps the column-level `PRIMARY KEY` (SQLite `INTEGER PRIMARY KEY
  AUTOINCREMENT` included); two or more keys emit a table-level `PRIMARY KEY (a, b)` and no
  column-level constraint.
- Statements: `select_by_key_statement`, `delete_by_key_statement` (with and without the
  concurrency token), `update_statement` and `update_partial_statement` build the full key
  predicate in descriptor order; `ON CONFLICT (a, b)` and the SQL Server `MERGE ... ON` list all
  key columns. `OUTPUT INSERTED`/`RETURNING` stay limited to a single generated key column.
- `mapper.hpp` exposes the entity's key as `row_key`; single-column helpers keep their signature.

Acceptance: core statement tests for string and composite keys (DDL, insert, select, update,
delete, upsert, bind order); a live SQLite test that migrates, inserts, finds, updates and deletes
by a composite and by a string key.

### P0.2 Context identity map and concurrency

**Status:** implemented (row_key identity map, key-predicate concurrency entries, include grouping).

- `set_state<T>` keys `identity` by `row_key`; `find` accepts `row_key` and the existing
  single-value convenience overloads; `add`/`attach`/`refresh`/`remove`/`save`/`delete_removed`
  use the general key. "Key is unset" becomes an explicit generated-key check, not `key == 0`.
- `concurrency_entry::key` becomes a `row_key` (diagnostic string preserved); the concurrency
  `WHERE` binds every key part in order before the token.
- `include`/`include_parent` group by `row_key`, so single-column and composite keys work without
  the text-concatenation collisions of `canonical_key`.

Acceptance: context tests for string-PK CRUD and identity-map reuse, composite-PK
add/find/attach/remove/refresh, composite concurrency conflict entries, and no change for
integral auto-increment models.

### P0.3 Migration, introspection and validation

**Status:** implemented for creating table-level keys, key add/drop and introspection; altering the columns of an existing key (P0.3b2) is still open.

- `entityfy-migrate`'s table model carries the ordered primary-key column list, not only
  per-column flags; JSON round-trips it and old files without the list are read as the current
  per-column flags.
- Diff/plan gains primary-key add/drop/alter operations. SQLite uses its rebuild path; PostgreSQL
  emits `ALTER TABLE ... ADD/DROP CONSTRAINT`.
- Introspection reads the key order (SQLite `PRAGMA table_xinfo` ordinal, PostgreSQL
  `pg_index.indkey`); validation compares the key set, not just per-column flags.
- MySQL/SQL Server stay compile-clean: composite keys are rejected by the migrate provider with a
  clear "not supported for this provider" error until the provider work is unpaused.

Acceptance: migrate tests for a new composite-key table, a key change on SQLite (rebuild) and
PostgreSQL (constraint), JSON/scaffold round-trip, and the old single-key migrations unchanged.

### P0.4 Tests and docs

**Status:** implemented (live SQLite/PostgreSQL key coverage; docs updated).

Live SQLite and PostgreSQL coverage for every key shape; docs (`core.md`, `context.md`,
`migration.md`, README) describe the key rules, the composite-FK status and the
single-integral restriction on identity columns. Benchmarks note any measured cost of the general
key path on single-column models.

## Phase 1 — lazy loading

### P1.1 `lazy<T>` core

**Status:** implemented (commit e7f0a0c); unit tests cover loads, cache, copies and lifetimes.

New `entityfy-core/include/entityfy/lazy.hpp`: `lazy<T>` for a reference (`T*`) and
`lazy<std::vector<T>>` for a collection, a `detail::lazy_link` (owner pointer, generation, alive
flag), `lazy_error` with `not_attached`, `context_destroyed`, `context_cleared`, `not_found`.
`validate_model` rejects unignored lazy members and lazy members inside owned types.

### P1.2 Context binding and lifetimes

**Status:** implemented (commits a20ed7b, 4c529a8). Handles bind on add/attach/refresh/save and on identity-map hits; save and rollback reset/rebind the touched entity. Composite owner keys are rejected until P1.5.

`db_context` owns the link; `clear()` bumps the generation; the destructor clears `alive`.
`set_state<T>` binds/rebinds handles in `add`, `attach` (only when not already attached),
`refresh` (resetting the cache), generated-key insert, and after saving an entity whose foreign
key changed. Rollback resets caches. Binding stores key values, never entity addresses.

### P1.3 Loaders and include integration

**Status:** implemented (commits a20ed7b, f31554b): collection and reference loaders, include cache fill, no-tracking stays unbound.

1:N loads by parent key, N:1 by the child's foreign key; rows go through `materialize_row` and
attach, so identity-map semantics match `include`. `include`/`include_parent` fill the handle
cache (`mark_loaded`) without a second query. No-tracking paths do not bind.

### P1.4 Tests and docs

**Status:** implemented (lazy_loading scenario on SQLite and PostgreSQL; README/context/roadmap).

Tracked 1:N/N:1, one query per load, include cache, no-tracking rejection, destroyed/cleared
context errors, transaction visibility and rollback reset, dirty-tracking neutrality, refreshed
foreign key, added entity after key generation, pool lease invalidation. Document the lifetime
rules and the EF differences (explicit handle instead of a proxy).

### P1.5 Composite foreign keys

Multi-column `references`, relation descriptors with column lists, `include`/lazy grouping and
`include_parent` across composite keys. Until this slice lands, composite keys work for CRUD but
navigation helpers reject composite foreign keys with a clear error.

## Phase 2 — async I/O

### P2.1 `task<T>` and executors (no database)

**Status:** implemented (commit 1131408): `task<T>`/`task<void>`, cancellation hook,
`on_complete`, flattened `then`, inline and bounded pool executors, 19 tests.

`entityfy::task<T>`/`task<void>` with a completion callback, cancellation hook and `sync_wait`
for tests/CLI; a bounded thread-pool executor and a single-thread reactor abstraction. Unit tests
for values, exceptions, move-only results, nested awaits, destruction of unused tasks and
cancellation.

### P2.2 PostgreSQL native async

Follow the audit decision B: the PostgreSQL backend owns one raw `PGconn*` and serves both the
sync and async paths through shared parameter/result/SQLSTATE helpers (taopq is removed from the
dependency once the port is proven). Async flow: `PQsendQueryParams`/`PQsendQueryPrepared` →
`PQflush` → `PQsocketPoll` → `PQconsumeInput`/`PQisBusy` → `PQgetResult`; pipeline batches send
all statements, then `PQsendPipelineSync`, drain `PGRES_PIPELINE_ABORTED` on error, exit pipeline
mode. One in-flight operation per connection; a second call is an explicit error. Cancellation
uses libpq 17+ `PQcancelCreate/Start/Poll/Finish`; timeout marks the connection broken, is not
retried and is not a `transient_error`. `find_package(PostgreSQL 17 REQUIRED)` (or feature-macro
fallbacks) is enforced.

### P2.3 Client and table surface

`async_capability async_mode()`, `execute_async`, `query_async`, `query_each_async`,
`execute_batch_async`, `transaction_async`, and `table_client` async CRUD mirroring the sync
methods. The async API uses the same `statement` values, so SQL generation and the key work are
shared.

### P2.4 SQLite facade

Bounded worker pool with one serialized queue per connection (no thread per call), returning the
same `task<T>` and reporting `thread_offload`. Cancellation is cooperative between statements
only; previous-statement completion cannot be interrupted. `:memory:` databases remain
single-context by definition.

### P2.5 Context and pool

`save_changes_async` returns `task<save_result>`; retry delays await the executor instead of
blocking a thread; a save gate rejects concurrent sync/async saves. Phase 1 lands the offload
path, phase 2 the native path (`saveable` async methods, `save_changes_once_async`,
`transaction_async`). `db_context_pool` gains `acquire_async` and discards broken connections
after timeout/cancel.

### P2.6 Tests and docs

`pg_sleep` interleaving proves a running coroutine does not block the caller; the same test over
two connections proves the reactor is shared; a hundred async queries keep the thread count
constant; single-in-flight, cancellation, timeout, retry classification, pipeline failure drain,
SQLite facade serialization and async pool behavior are covered. Docs state the capability
difference and the single-owner rule.

## Phase 3 — ODBC decision (no code)

Recorded for later, not started: ODBC remains the SQL Server transport only. Its known gaps are
connection-string escaping, the useless `mssql://` alias, `BEGIN`-based transactions without
isolation/MARS attributes, native-code-only error classification (1205 deadlock and Azure
transient codes missing), `OUTPUT`/`MERGE` key limitations, and SQL-dump-only backups. Finishing
that provider is a separate project behind the Deferred section; it must not be pulled into the
SQLite/PostgreSQL work.

## Verification

Every slice runs the affected suites in `build/dev` (SQLite) and, when PostgreSQL is involved,
`build/release` with `ENTITYFY_PG_URI` and `ENTITYFY_REQUIRE_PG=1`. Key and async slices add
focused tests before the full suite. The final matrix is the full CTest run plus
`build/mysql`/`build/sqlserver` compilation, since shared headers change.

## Risks

- The key generalisation touches public signatures (`find`, `concurrency_entry::key`); templated
  overloads keep the old call sites source-compatible.
- Composite keys and identity columns are mutually exclusive; this is enforced at compile time.
- Lazy handles never own entities; context lifetime and single-thread rules apply unchanged.
- Async PostgreSQL replaces the driver path; the sync suite must stay green at every slice.
- The SQL Server provider shares the core descriptor and must keep compiling without gaining
  behaviour.
