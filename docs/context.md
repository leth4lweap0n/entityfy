# entityfy-context

The Entity Framework Core style layer: a `db_context` (the `DbContext` equivalent) owns
`db_set<T>` handles (the `DbSet<T>` equivalents) and the change tracker. SQL generation and
mapping come from `entityfy-core`; this module answers "what changed" and "what is this
entity".

## Declaring a context

C++26 reflection cannot synthesize a member, so each entity set is declared once — exactly
like EF Core requires `public DbSet<User> Users => Set<User>();`. Everything inside the set
(table name, columns, primary key, foreign keys) is derived from the model through reflection.

```cpp
struct user
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::not_null]]
    std::string email;

    [[= entityfy::ignore]]
    std::vector<post*> posts;   // navigation property
};

struct post
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::references{"users", "id", "CASCADE"}]]
    std::int64_t user_id = 0;

    [[= entityfy::not_null]]
    std::string title;
};

struct app_context : entityfy::db_context
{
    using entityfy::db_context::db_context;

    entityfy::db_set<user> users{*this};
    entityfy::db_set<post> posts{*this};
};
```

## Basic usage

```cpp
app_context ctx{"sqlite://app.db"};

ctx.migrate_all();                 // both tables, parents first, order derived from reflection

ctx.users.add(user{.email = "ada@example.com", .name = "Ada"});
ctx.users.add(user{.email = "grace@example.com", .name = "Grace"});

const auto result = ctx.save_changes();   // single transaction: inserted / updated / removed
```

`save_changes()` runs in a single transaction; on error it rolls back and preserves the user's
pending edits. Generated keys are written back to the entity.

A context without declared members works too:

```cpp
entityfy::db_context ctx{"sqlite://app.db"};
auto users = ctx.set<user>();      // same tracker, same identity map
users.add(user{.email = "ada@example.com"});
ctx.save_changes();
```

## Reflection inside the context

`db_context::model()` exposes what reflection derived for every entity type the context knows:

```cpp
for(const auto& model: ctx.model())
{
    std::cout << model.table << ": " << model.columns.size() << " columns\n";
    for(const auto& referenced: model.foreign_keys)
    {
        std::cout << "  -> " << referenced << "\n";
    }
}
```

`migrate_all()` uses the same metadata to create tables parents-first:

- INSERT/UPDATE **parent-first** (even if the child set was opened first, the parent is written
  first).
- DELETE **child-first**; this way, in tables using `ON DELETE RESTRICT`, a parent and its child
  can be deleted in the same save.
- Tables are created in the same order.

## Change tracking

- Snapshot-based: an `original` copy of each entity is kept, and `save_changes()` writes only the
  changed columns (partial UPDATE).
- `to_list()`, `find()`, `first()` and `include()` return pointers into the tracker: those rows
  are materialized *and* attached, so they pay for the `original` copy and the identity map entry.
  `find`, `to_list`, `from_sql`, `include`, `include_parent` and the joins stream the result
  through `client::query_each` and attach row by row, so no `vector<row>` is built in between.
- `query().select(&T::a, &T::b)` returns `std::vector<std::tuple<...>>` of values and never
  touches the tracker (the equivalent of EF Core's `Select(x => new { ... })`).
- `as_no_tracking()` is the full-entity counterpart (EF Core's `AsNoTracking()`):

  ```cpp
  const auto users = ctx.users.as_no_tracking()
                         .where(entityfy::where(&user::name).eq("Ada"))
                         .order_by(&user::name)
                         .to_list();   // std::vector<user>, nothing attached
  ```

  Rows are materialized into the vector and never registered: no `original` snapshot and no
  identity map entry (measured: 332 ns/row against 460 ns/row tracked, see
  [benchmarks.md](benchmarks.md)). `first()` and `count()` are available too.

  `to_list()` returns an owning, **read-only** result (`no_tracking_result<T>`): `rows[0]` is
  `const T&`, so `rows[0].name = "x"` does not compile. `first()` returns a copy instead - editing
  that copy is ordinary C++ on a value and never reaches the database. That is the deliberate
  difference from EF Core's `AsNoTracking`, where the same mistake is a silent no-op.

  Eager loading works on the same builder and yields owning value pairs, so nothing points into
  the result:

  ```cpp
  const auto rows = ctx.users.as_no_tracking()
                        .order_by(&user::name)
                        .include<post>()          // [[= entityfy::references{"users", "id"}]]
                        .to_list();               // std::vector<std::pair<user, std::vector<post>>>
  ```

  A `std::vector<post>` navigation member is filled as well; pointer navigations
  (`std::vector<post*>`) stay empty because a pointer into an owning vector would dangle.
- state is read with `users.state(entity)`: `unchanged`, `added`, `modified`, `removed`,
  `detached`.
- `users.refresh(entity)` reloads the entity from the database.
- `users.detach(entity)` removes it from tracking.
- `ctx.clear()` drops all tracking state (the connection stays open) — the context pool uses
  this.
- **Manual change tracking** turns the snapshot scan off:

  ```cpp
  ctx.set_auto_detect(false);
  ada.name = "Ada Lovelace";
  ctx.users.mark_modified(ada);   // the only signal save_changes() uses now
  ctx.save_changes();
  ```

  With auto detection off, `save_changes()` visits only the marked entities (plus pending inserts
  and removals), so a one-row update no longer walks the tracked set: measured 5,5 µs at 50.000
  tracked rows against 10,6 ms with auto detection. The trade-off is explicit - an edit to an
  entity that was not marked is **not** written, and `state(entity)` reports it as `unchanged`.
  `set_auto_detect(true)` restores the EF Core style behaviour; a following save then picks up
  edits that were left unmarked.

`add` / `attach` / `find` / `to_list` return references into the tracker. Those references stay
valid across other inserts and deletes until the entity itself is removed from tracking or
`clear()` runs.

## Queries and relations

```cpp
auto selected = ctx.users.query()
                       .where(entityfy::where(&user::email).ends_with("@example.com"))
                       .order_by(&user::name)
                       .take(10)
                       .to_list();

auto joined = ctx.users.join<post>(&user::id, &post::user_id);                 // two tables
auto three  = ctx.posts.join<user, comment>(&post::user_id, &user::id,
                                            &comment::post_id, &post::id);    // three tables

auto children = ctx.users.include<post>({&ada, &grace});                      // 1:N, single query
auto parents  = ctx.posts.include_parent<user>({&post1, &post2});              // N:1
```

`include`/`include_parent` read the foreign key from the `references` annotation — no member
pointer, no manual join condition. When the parent declares
`[[= entityfy::ignore]] std::vector<Child*>`, `include<Child>` fills it with the tracked children
(changes made to the pointer collection are written by `save_changes()`); a `std::vector<Child>`
member receives a read-only copy snapshot.

`entityfy::where(...)` is the core-level predicate builder; it stays snake_case because it lives
in `entityfy-core`, next to the SQL generation it feeds.

Bulk operations bypass the change tracker:

```cpp
const auto removed = ctx.posts.query().where(entityfy::where(&post::id) == 7).remove_all();
const auto updated = ctx.posts.query().assign(&post::score, 10).update_all();
```

## Concurrency

Optimistic concurrency is opt-in through a token column: the member named `version`, or any
persisted integral member annotated with `[[= entityfy::concurrency_token]]`.

```cpp
struct user
{
    // ...
    [[= entityfy::not_null]]
    std::int64_t version = 0;

    // or: [[= entityfy::concurrency_token, = entityfy::not_null]] std::int64_t revision = 0;
};
```

Updates turn into

```sql
UPDATE "users" SET "name" = ?, "version" = "version" + 1
WHERE "id" = ? AND "version" = ?;
```

and deletes carry the token too, so a stale delete cannot silently win. When a statement affects
zero rows `entityfy::concurrency_error` is thrown with one entry per conflict:

| field | meaning |
|---|---|
| `table`, `key` | the row that conflicted |
| `token_column`, `original_token` | the token column and the stale value (monostate without a token) |
| `columns`, `database_values` | the row as it exists now; empty when it was deleted (`row_exists()`) |

Resolving a conflict:

```cpp
try
{
    ctx.save_changes();
}
catch(const entityfy::concurrency_error& error)
{
    const auto& entry = error.entries().front();

    // store wins: reload the row (values replaced, entity becomes unchanged)
    users.refresh(user);

    // client wins: keep the edit, take the database row as the new original snapshot
    const auto database_row = users.database_values(user);
    users.accept_changes(user, *database_row);
    ctx.save_changes();

    // merge: move only one member's original value to the database row
    users.accept_changes(user, *database_row, &user::email);
}
```

`attach(value, original)` preserves a separate snapshot for disconnected entities, so a stale
token survives the round trip and is checked on the next save. `save_changes()` still runs in one
transaction: a conflict rolls the whole batch back and restores the in-memory token values, so the
resolution above can be retried in the same context.

**Honest limits:** `remove_all()` / `update_all()` bypass the tracker and the token; the retry
policy covers transient errors only, a concurrency conflict is never retried automatically; the
token must map to a single persisted column.

## Batch, retry, transaction

```cpp
ctx.set_batch_parameter_limit(500);      // multi-row INSERT splitting
ctx.set_retry_policy(entityfy::retry_policy{.attempts = 3, .delay = std::chrono::milliseconds{50}});

ctx.transaction([&] {
    ctx.users.add(user{.email = "tx@example.com", .name = "Tx"});
    ctx.save_changes();                // nested savepoint
});
```

## Asynchronous saving

```cpp
auto pending = ctx.save_changes_async();
const auto result = pending.get();     // std::future<save_result>
```

`save_changes_async()` runs the same save work on a worker thread; saves are serialized by the
mutex inside the context, so two concurrent saves cannot interleave.
**Honest limit:** this is not I/O asynchrony — the SQLite and PostgreSQL drivers run
synchronously; the gain is only in reducing how long the calling thread waits. The context cannot
be used from other threads while a save is in progress (the same rule as EF Core).

## Context pool

```cpp
entityfy::db_context_pool pool{"sqlite://app.db", 8};

{
    auto lease = pool.acquire();
    lease->migrate<user>();
    auto users = lease->set<user>();
    users.add(user{.email = "ada@example.com", .name = "Ada"});
    lease->save_changes();
}   // lease released: change tracker cleared, context returns to the pool
```

The pool must outlive the leases. On a context returning to the pool, the change tracker is
cleared; the logger, retry policy, and batch limit are kept.

## Naming

The context module mirrors Entity Framework Core's *shape* — `db_context` / `db_set<T>` (the
`DbContext` / `DbSet<T>` equivalents) — but keeps the library's single convention: everything is
snake_case, types and members alike (`save_changes`, `add`, `find`, `include`, `query`,
`migrate_all`). `entityfy-core` uses the same convention (`client`, `table_client`,
`describe_model`, `entityfy::where`); the context layer is built on top of it and `database()` is
the escape hatch to it.
