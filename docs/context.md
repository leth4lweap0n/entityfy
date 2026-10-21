# entityfy-context

A DbContext-like change tracking layer. It builds on `entityfy::core`; it takes SQL generation
from core and answers only the question "what changed".

## Basic usage

```cpp
entityfy::context ctx{"sqlite://app.db"};
ctx.migrate<user>();
ctx.migrate<post>();

auto users = ctx.set<user>();
auto& ada = users.add(user{.email = "ada@example.com", .name = "Ada"});

auto posts = ctx.set<post>();
posts.add(post{.user_id = ada.id, .title = "first"});

const auto result = ctx.save_changes();
// result.inserted / updated / removed
```

`save_changes()` runs in a single transaction; on error it rolls back and preserves the user's
pending edits. Generated keys are written back to the entity.

## Change tracking

- Snapshot-based: an `original` copy of each entity is kept, and `save_changes()` writes only the
  changed columns (partial UPDATE).
- The state is read with `ctx.state(entity)`: `unchanged`, `added`, `modified`, `removed`,
  `detached`.
- `ctx.refresh(entity)` reloads the entity from the database.
- `ctx.detach(entity)` removes it from tracking.
- `ctx.clear()` drops all tracking state (the connection stays open) — the context pool uses
  this.

## Queries and relations

```cpp
auto selected = users.query()
                     .where(entityfy::where(&user::email).ends_with("@example.com"))
                     .order_by(&user::name)
                     .take(10)
                     .to_list();

auto joined = users.join<post>(&user::id, &post::user_id);                 // two tables
auto three  = posts.join<user, comment>(&post::user_id, &user::id,
                                         &comment::post_id, &post::id);      // three tables

auto children = users.include<post>({&ada, &grace}, &post::user_id);        // 1:N, single query
auto parents  = posts.include_parent<user>({&post1, &post2}, &post::user_id); // N:1
```

If the parent has an `[[= entityfy::ignore]] std::vector<Child*>` member, `include<Child>` fills
it with the tracked children (changes made to the pointer collection are written by
`save_changes()`). If the member is `std::vector<Child>`, it is filled as a read-only copy
snapshot.

Bulk operations bypass the change tracker:

```cpp
const auto removed = posts.query().where(entityfy::where(&post::id) == 7).remove_all();
const auto updated = posts.query().assign(&post::score, 10).update_all();
```

## Dependency order

`save_changes()` orders entity sets according to the foreign key graph:

- INSERT/UPDATE **parent-first** (even if the child set was opened first, the parent is written
  first).
- DELETE **child-first**; this way, in tables using `ON DELETE RESTRICT`, a parent and its child
  can be deleted in the same save.

## Batch, retry, transaction

```cpp
ctx.set_batch_parameter_limit(500);   // multi-row INSERT splitting
ctx.set_retry_policy(entityfy::retry_policy{.attempts = 3, .delay = std::chrono::milliseconds{50}});

ctx.transaction([&] {
    users.add(user{.email = "tx@example.com", .name = "Tx"});
    ctx.save_changes();               // nested savepoint
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
entityfy::context_pool pool{"sqlite://app.db", 8};

{
    auto lease = pool.acquire();
    lease->migrate<user>();
    auto users = lease->set<user>();
    users.add(user{.email = "ada@example.com", .name = "Ada"});
    lease->save_changes();
}   // lease released: change tracker cleared, context returns to the pool
```

The pool must outlive the leases. On a context returning to the pool, the logger, retry policy,
and batch limit are reset.
