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

    entityfy::db_set<user> Users{*this};
    entityfy::db_set<post> Posts{*this};
};
```

## Basic usage

```cpp
app_context ctx{"sqlite://app.db"};

ctx.MigrateAll();                 // both tables, parents first, order derived from reflection

ctx.Users.Add(user{.email = "ada@example.com", .name = "Ada"});
ctx.Users.Add(user{.email = "grace@example.com", .name = "Grace"});

const auto result = ctx.SaveChanges();   // single transaction: inserted / updated / removed
```

`SaveChanges()` runs in a single transaction; on error it rolls back and preserves the user's
pending edits. Generated keys are written back to the entity.

A context without declared members works too:

```cpp
entityfy::db_context ctx{"sqlite://app.db"};
auto users = ctx.Set<user>();      // same tracker, same identity map
users.Add(user{.email = "ada@example.com"});
ctx.SaveChanges();
```

## Reflection inside the context

`db_context::Model()` exposes what reflection derived for every entity type the context knows:

```cpp
for(const auto& model: ctx.Model())
{
    std::cout << model.table << ": " << model.columns.size() << " columns\n";
    for(const auto& referenced: model.foreign_keys)
    {
        std::cout << "  -> " << referenced << "\n";
    }
}
```

`MigrateAll()` uses the same metadata to create tables parents-first:

- INSERT/UPDATE **parent-first** (even if the child set was opened first, the parent is written
  first).
- DELETE **child-first**; this way, in tables using `ON DELETE RESTRICT`, a parent and its child
  can be deleted in the same save.
- Tables are created in the same order.

## Change tracking

- Snapshot-based: an `original` copy of each entity is kept, and `SaveChanges()` writes only the
  changed columns (partial UPDATE).
- State is read with `Users.State(entity)`: `unchanged`, `added`, `modified`, `removed`,
  `detached`.
- `Users.Refresh(entity)` reloads the entity from the database.
- `Users.Detach(entity)` removes it from tracking.
- `ctx.Clear()` drops all tracking state (the connection stays open) — the context pool uses
  this.

`Add` / `Attach` / `Find` / `ToList` return references into the tracker. Those references stay
valid across other inserts and deletes until the entity itself is removed from tracking or
`Clear()` runs.

## Queries and relations

```cpp
auto selected = ctx.Users.Query()
                       .Where(entityfy::where(&user::email).ends_with("@example.com"))
                       .OrderBy(&user::name)
                       .Take(10)
                       .ToList();

auto joined = ctx.Users.Join<post>(&user::id, &post::user_id);                 // two tables
auto three  = ctx.Posts.Join<user, comment>(&post::user_id, &user::id,
                                            &comment::post_id, &post::id);    // three tables

auto children = ctx.Users.Include<post>({&ada, &grace});                      // 1:N, single query
auto parents  = ctx.Posts.IncludeParent<user>({&post1, &post2});              // N:1
```

`Include`/`IncludeParent` read the foreign key from the `references` annotation — no member
pointer, no manual join condition. When the parent declares
`[[= entityfy::ignore]] std::vector<Child*>`, `Include<Child>` fills it with the tracked children
(changes made to the pointer collection are written by `SaveChanges()`); a `std::vector<Child>`
member receives a read-only copy snapshot.

`entityfy::where(...)` is the core-level predicate builder; it stays snake_case because it lives
in `entityfy-core`, next to the SQL generation it feeds.

Bulk operations bypass the change tracker:

```cpp
const auto removed = ctx.Posts.Query().Where(entityfy::where(&post::id) == 7).RemoveAll();
const auto updated = ctx.Posts.Query().Assign(&post::score, 10).UpdateAll();
```

## Batch, retry, transaction

```cpp
ctx.SetBatchParameterLimit(500);      // multi-row INSERT splitting
ctx.SetRetryPolicy(entityfy::retry_policy{.attempts = 3, .delay = std::chrono::milliseconds{50}});

ctx.Transaction([&] {
    ctx.Users.Add(user{.email = "tx@example.com", .name = "Tx"});
    ctx.SaveChanges();                // nested savepoint
});
```

## Asynchronous saving

```cpp
auto pending = ctx.SaveChangesAsync();
const auto result = pending.get();     // std::future<save_result>
```

`SaveChangesAsync()` runs the same save work on a worker thread; saves are serialized by the
mutex inside the context, so two concurrent saves cannot interleave.
**Honest limit:** this is not I/O asynchrony — the SQLite and PostgreSQL drivers run
synchronously; the gain is only in reducing how long the calling thread waits. The context cannot
be used from other threads while a save is in progress (the same rule as EF Core).

## Context pool

```cpp
entityfy::db_context_pool pool{"sqlite://app.db", 8};

{
    auto lease = pool.Acquire();
    lease->Migrate<user>();
    auto users = lease->Set<user>();
    users.Add(user{.email = "ada@example.com", .name = "Ada"});
    lease->SaveChanges();
}   // lease released: change tracker cleared, context returns to the pool
```

The pool must outlive the leases. On a context returning to the pool, the change tracker is
cleared; the logger, retry policy, and batch limit are kept.

## Naming

The context module mirrors Entity Framework Core: `db_context` / `db_set<T>` (the
`DbContext` / `DbSet<T>` equivalents) with PascalCase members (`SaveChanges`, `Add`, `Find`,
`Include`, `Query`). `entityfy-core` keeps its own snake_case SQL/mapping surface
(`client`, `table_client`, `describe_model`, `entityfy::where`); the context layer is built on
top of it and `Database()` is the escape hatch to it.
