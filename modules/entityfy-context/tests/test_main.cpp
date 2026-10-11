#include <entityfy/context.hpp>
#include <entityfy/pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

namespace
{

enum class status
{
    passed,
    skipped,
    failed,
};

void require(bool condition, std::string_view message)
{
    if(!condition)
    {
        throw std::runtime_error{std::string{message}};
    }
}

void require_not(bool condition, std::string_view message)
{
    require(!condition, message);
}

struct [[= entityfy::table{"rc_posts"}]] rc_post
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"rc_users", "id", "CASCADE"}]]
    std::int64_t user_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::not_null]]
    std::int64_t score = 0;
};

struct [[= entityfy::table{"rc_comments"}]] rc_comment
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"rc_posts", "id", "CASCADE"}]]
    std::int64_t post_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string body;
};

struct [[= entityfy::table{"rc_users"}]] rc_user
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::not_null]]
    std::int64_t version = 0;

    [[= entityfy::ignore]]
    std::vector<rc_post*> posts;
};

static_assert(entityfy::detail::is_ignored_member(^^rc_user::posts), "posts must be ignored");

struct [[= entityfy::table{"rc_tags"}]] rc_tag
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{50}]]
    std::string name;

    [[= entityfy::json]]
    std::string payload = "{}";
};

struct [[= entityfy::table{"rc_parents"}]] rc_parent
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{50}]]
    std::string name;
};

struct [[= entityfy::table{"rc_children"}]] rc_child
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"rc_parents", "id", "RESTRICT"}]]
    std::int64_t parent_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{50}]]
    std::string label;
};

std::int64_t as_integer(const entityfy::sql_value& value)
{
    if(const auto* number = std::get_if<std::int64_t>(&value))
    {
        return *number;
    }

    if(const auto* text = std::get_if<std::string>(&value))
    {
        return std::stoll(*text);
    }

    throw std::runtime_error{"expected integer value"};
}

std::size_t occurrences(const std::string& text, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t position = 0;

    while((position = text.find(needle, position)) != std::string::npos)
    {
        ++count;
        position += needle.size();
    }

    return count;
}

bool postgres_required()
{
    const char* value = std::getenv("ENTITYFY_REQUIRE_PG");
    return value != nullptr && std::string{value} == "1";
}

status run_sqlite_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();
    ctx.migrate<rc_post>();
    ctx.migrate<rc_comment>();
    ctx.migrate<rc_tag>();

    auto users = ctx.set<rc_user>();

    auto& ada = users.add(rc_user{.email = "ada@test.com", .name = "Ada"});
    auto& grace = users.add(rc_user{.email = "grace@test.com", .name = "Grace"});

    auto saved = ctx.save_changes();
    require(saved.inserted == 2, "insert count");
    require(ada.id != 0 && grace.id != 0, "generated ids assigned");
    require(users.find(ada.id) == &ada, "identity map keeps instance");

    require(users.query().sum(&rc_user::id) == std::optional<std::int64_t>{3}, "aggregate sum");
    require(users.query().avg(&rc_user::id) == std::optional<double>{1.5}, "aggregate avg");
    require(users.query().min(&rc_user::id) == std::optional<std::int64_t>{1}, "aggregate min");
    require(users.query().max(&rc_user::id) == std::optional<std::int64_t>{2}, "aggregate max");
    require(users.query()
                    .where(entityfy::where(&rc_user::email).ends_with("grace@test.com"))
                    .sum(&rc_user::id) == std::optional<std::int64_t>{2},
            "filtered aggregate sum");

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    const auto grouped = ctx.query(
        "SELECT \"name\", COUNT(*) AS \"n\" FROM \"rc_users\" GROUP BY \"name\" ORDER BY \"name\"");
    require(grouped.size() == 2, "raw group by row count");
    require(std::get<std::string>(grouped[0][0]) == "Ada", "raw group by name");
    require(as_integer(grouped[0][1]) == 1, "raw group by count");

    const auto filtered = ctx.query(
        "SELECT \"email\" FROM \"rc_users\" WHERE \"name\" = ?",
        {entityfy::to_sql_value(std::string{"Grace"})});
    require(filtered.size() == 1 && std::get<std::string>(filtered[0][0]) == "grace@test.com",
            "raw query with binds");

    statements.clear();
    ada.name = "Ada Lovelace";
    saved = ctx.save_changes();

    require(saved.updated == 1, "single update tracked");
    require(statements.size() == 1, "single update statement");
    require(statements.front().find("\"name\" = ?") != std::string::npos, "changed column is in update");
    require(statements.front().find("\"email\" = ?") == std::string::npos, "unchanged column is not in update");

    const auto* fetched = users.find(ada.id);
    require(fetched != nullptr && fetched->name == "Ada Lovelace", "database updated");

    statements.clear();
    saved = ctx.save_changes();
    require(saved.updated == 0, "no-op save produces no update");
    require(statements.empty(), "no-op save produces no SQL");

    auto rows = ctx.db().table<rc_user>().find_many();
    require(rows.size() == 2, "two rows remain");

    auto posts = ctx.set<rc_post>();
    auto& first_post = posts.add(rc_post{.user_id = ada.id, .title = "first", .score = 10});
    auto& second_post = posts.add(rc_post{.user_id = ada.id, .title = "second", .score = 20});
    auto& third_post = posts.add(rc_post{.user_id = grace.id, .title = "third", .score = 5});

    statements.clear();
    const auto posts_saved = ctx.save_changes();
    require(posts_saved.inserted == 3, "posts inserted");

    auto comments = ctx.set<rc_comment>();
    comments.add(rc_comment{.post_id = first_post.id, .body = "one"});
    comments.add(rc_comment{.post_id = second_post.id, .body = "two"});

    const auto comments_saved = ctx.save_changes();
    require(comments_saved.inserted == 2, "comments inserted");

    const auto joined_three = posts.join<rc_user, rc_comment>(
        &rc_post::user_id, &rc_user::id,
        &rc_comment::post_id, &rc_post::id);
    require(joined_three.size() == 2, "three-way join row count");
    require(std::get<0>(joined_three[0])->id == first_post.id, "three-way join left row");
    require(std::get<1>(joined_three[0])->id == ada.id, "three-way join middle row");
    require(std::get<2>(joined_three[0])->post_id == first_post.id, "three-way join right row");

    const auto grouped_multi = posts.query().group_count(&rc_post::user_id, &rc_post::score);
    require(grouped_multi.size() == 3, "multi-key group count");

    const auto grouped_users = users.query().group_count(&rc_user::name, &rc_user::email);
    require(grouped_users.size() == 2, "string-key grouped count");

    const auto grouped_having = posts.query().group_count_having(
        entityfy::condition{.sql = "COUNT(*) >= 2"}, &rc_post::user_id);
    require(grouped_having.size() == 1 && grouped_having.begin()->second == 2, "grouped count with having");
    require(std::ranges::any_of(statements, [](const std::string& statement) {
                return statement.find("INSERT INTO") != std::string::npos;
            }),
            "insert logged");

    const auto selected = users.query()
                              .where(entityfy::where(&rc_user::email).ends_with("@test.com"))
                              .order_by(&rc_user::name)
                              .take(1)
                              .to_list();
    require(selected.size() == 1 && selected.front() == &ada, "query filter/order/take");
    require(users.query().count() == 2, "query count");

    const auto joined = users.join<rc_post>(&rc_user::id, &rc_post::user_id);
    require(joined.size() == 3, "join row count");
    std::size_t ada_posts = 0;
    for(const auto& [user_ptr, post_ptr]: joined)
    {
        require(post_ptr->user_id == user_ptr->id, "join predicate");
        if(user_ptr == &ada)
        {
            ++ada_posts;
        }
    }
    require(ada_posts == 2, "join parent matching");

    const auto counts = posts.query().count_by(&rc_post::user_id);
    require(counts.size() == 2 && counts[0].first == ada.id && counts[0].second == 2 &&
                counts[1].first == grace.id && counts[1].second == 1,
            "count_by");

    const auto sums = posts.query().sum_by(&rc_post::user_id, &rc_post::score);
    require(sums.size() == 2 && sums[0].second == 30 && sums[1].second == 5, "sum_by");

    const auto averages = posts.query().avg_by(&rc_post::user_id, &rc_post::score);
    require(averages.size() == 2 && averages[0].second == 15.0 && averages[1].second == 5.0, "avg_by");

    const auto minimums = posts.query().min_by(&rc_post::user_id, &rc_post::score);
    require(minimums.size() == 2 && minimums[0].second == 10 && minimums[1].second == 5, "min_by");

    const auto maximums = posts.query().max_by(&rc_post::user_id, &rc_post::score);
    require(maximums.size() == 2 && maximums[0].second == 20 && maximums[1].second == 5, "max_by");

    const auto included = users.include<rc_post>({&ada, &grace}, &rc_post::user_id);
    require(included.size() == 2, "include parent count");
    require(included[0].first == &ada && included[0].second.size() == 2, "include children of ada");
    require(included[1].first == &grace && included[1].second.size() == 1, "include children of grace");
    require(ada.posts.size() == 2 && grace.posts.size() == 1, "include fills navigation collections");
    require(ada.posts[0] == included[0].second[0], "navigation points at tracked child entities");

    const auto with_parents =
        posts.include_parent<rc_user>({&first_post, &second_post, &third_post}, &rc_post::user_id);
    require(with_parents.size() == 3, "include_parent count");
    require(with_parents[0].second == &ada && with_parents[2].second == &grace, "include_parent mapping");

    rc_post unsaved_child;
    unsaved_child.title = "unsaved";
    const auto unsaved_parents = posts.include_parent<rc_user>({&unsaved_child}, &rc_post::user_id);
    require(unsaved_parents.size() == 1 && unsaved_parents.front().second == nullptr,
            "include_parent handles unsaved child");

    const auto projected = users.query().order_by(&rc_user::name).select(&rc_user::id, &rc_user::name);
    require(projected.size() == 2, "projection count");
    require(std::get<0>(projected[0]) == ada.id && std::get<1>(projected[0]) == "Ada Lovelace",
            "projection values");

    const auto raw = users.from_sql(
        "SELECT \"id\", \"email\", \"name\", \"version\" FROM \"rc_users\" WHERE \"email\" = 'ada@test.com'");
    require(raw.size() == 1 && raw.front() == &ada, "raw sql materialization");

    statements.clear();
    users.remove(*fetched);
    saved = ctx.save_changes();
    require(saved.removed == 1, "delete tracked");
    require(std::ranges::any_of(statements, [](const std::string& statement) {
                return statement.find("DELETE FROM") != std::string::npos;
            }),
            "delete logged");
    require(ctx.db().table<rc_user>().count() == 1, "row deleted from database");
    require(ctx.db().table<rc_post>().count() == 1, "delete cascade removed posts");

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "alpha", .payload = "{\"a\":1}"});
    tags.add(rc_tag{.id = 2, .name = "beta"});
    ctx.save_changes();

    auto* alpha = tags.find(1);
    require(alpha != nullptr && alpha->payload == "{\"a\":1}", "json payload reaches identity map");

    const auto json_rows = ctx.query("SELECT \"payload\" FROM \"rc_tags\" WHERE \"id\" = 1");
    require(json_rows.size() == 1 && std::get<std::string>(json_rows.front().front()) == "{\"a\":1}",
            "json column round trip from database");

    const auto bulk_updated = tags.query()
                                  .where(entityfy::where(&rc_tag::id) == 2)
                                  .assign(&rc_tag::name, "beta2")
                                  .update_all();
    require(bulk_updated == 1, "query update_all affected one row");

    auto* beta = tags.find(2);
    require(beta != nullptr, "bulk updated row is tracked");
    tags.refresh(*beta);
    require(beta->name == "beta2", "query update_all persisted new value");

    const auto bulk_removed = tags.query().where(entityfy::where(&rc_tag::name) == "alpha").remove_all();
    require(bulk_removed == 1, "query remove_all affected one row");
    require(tags.query().count() == 1, "query remove_all left one row");

    return status::passed;
}

status run_rollback_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();
    auto& first = users.add(rc_user{.email = "first@test.com", .name = "First"});
    auto& second = users.add(rc_user{.email = "second@test.com", .name = "Second"});
    ctx.save_changes();

    first.email = "duplicate@test.com";
    second.email = "duplicate@test.com";

    bool failed = false;
    try
    {
        ctx.save_changes();
    }
    catch(const std::exception&)
    {
        failed = true;
    }

    require(failed, "duplicate unique value fails save");
    require(first.email == "duplicate@test.com" && second.email == "duplicate@test.com",
            "pending edits survive rollback");

    auto rows = ctx.db().table<rc_user>().find_many();
    require(rows.size() == 2, "row count unchanged after rollback");

    for(const auto& row: rows)
    {
        require_not(row.email == "duplicate@test.com", "rollback reverted database writes");
    }

    second.email = "second2@test.com";
    const auto saved = ctx.save_changes();
    require(saved.updated == 2, "retry after fixing duplicate updates both rows");

    return status::passed;
}

status run_concurrency_checks()
{
    const char* path = "rc_concurrency.db";
    std::filesystem::remove(path);

    {
    entityfy::context first_ctx{"sqlite://rc_concurrency.db"};
    first_ctx.migrate<rc_user>();

    auto first_users = first_ctx.set<rc_user>();
    auto& created = first_users.add(rc_user{.email = "concurrent@test.com", .name = "Initial"});
    first_ctx.save_changes();

    entityfy::context second_ctx{"sqlite://rc_concurrency.db"};
    auto second_users = second_ctx.set<rc_user>();

    auto* first_view = first_users.find(created.id);
    auto* second_view = second_users.find(created.id);
    require(first_view != nullptr && second_view != nullptr, "both contexts load entity");

    first_view->name = "First";
    first_ctx.save_changes();

    second_view->name = "Second";

    bool conflict = false;
    try
    {
        second_ctx.save_changes();
    }
    catch(const std::exception&)
    {
        conflict = true;
    }

    require(conflict, "stale version causes conflict");

    const auto rows = first_ctx.db().table<rc_user>().find_many();
    require(rows.size() == 1 && rows.front().name == "First", "winner value kept");
    require(rows.front().version == 1, "version incremented once");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_multi_conflict_checks()
{
    const char* path = "rc_multi_conflict.db";
    std::filesystem::remove(path);

    {
        entityfy::context writer{"sqlite://rc_multi_conflict.db"};
        writer.migrate<rc_user>();

        auto writer_set = writer.set<rc_user>();
        auto& first = writer_set.add(rc_user{.email = "a@multi.test", .name = "A"});
        auto& second = writer_set.add(rc_user{.email = "b@multi.test", .name = "B"});
        writer.save_changes();

        entityfy::context stale{"sqlite://rc_multi_conflict.db"};
        auto stale_set = stale.set<rc_user>();
        auto* stale_first = stale_set.find(first.id);
        auto* stale_second = stale_set.find(second.id);
        require(stale_first != nullptr && stale_second != nullptr, "stale context loads both entities");

        stale_first->name = "A1";
        stale_second->email = "a@multi.test"; // unique violation: collides with A's email

        bool failed = false;
        try
        {
            stale.save_changes();
        }
        catch(const std::exception&)
        {
            failed = true;
        }

        require(failed, "multi-entity save fails");
        require(stale_first->version == 0, "first entity version restored after rollback");
        require(stale_second->version == 0, "second entity version unchanged");

        auto rows = writer.db().table<rc_user>().find_many();
        for(const auto& row: rows)
        {
            require(row.version == 0, "database version rolled back");
            require(row.email != "a@multi.test" || row.name == "A", "database email unchanged for second");
        }

        stale_second->email = "b2@multi.test";
        const auto retried = stale.save_changes();
        require(retried.updated == 2, "retry updates both entities");

        rows = writer.db().table<rc_user>().find_many();
        for(const auto& row: rows)
        {
            require(row.version == 1, "version incremented once after retry");
        }
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_tracking_edge_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();

    auto& explicit_id = users.add(rc_user{.id = 99, .email = "explicit@test.com", .name = "Explicit"});
    (void)explicit_id;

    bool rejected = false;
    try
    {
        ctx.save_changes();
    }
    catch(const std::exception&)
    {
        rejected = true;
    }

    require(rejected, "explicit id on auto-increment model is rejected");
    require(ctx.db().table<rc_user>().count() == 0, "no row inserted for rejected explicit id");

    users.remove(explicit_id);

    auto& added = users.add(rc_user{.email = "cancel@test.com", .name = "Cancel"});
    users.remove(added);

    const auto saved = ctx.save_changes();
    require(saved.inserted == 0 && saved.removed == 0, "cancelled add is not persisted");
    require(ctx.db().table<rc_user>().count() == 0, "cancelled add leaves database empty");

    return status::passed;
}

status run_transaction_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();

    ctx.transaction([&] {
        users.add(rc_user{.email = "committed@test.com", .name = "Committed"});
        ctx.save_changes();
    });

    require(ctx.db().table<rc_user>().count() == 1, "transaction commit persists");

    bool rolled_back = false;
    try
    {
        ctx.transaction([&] {
            users.add(rc_user{.email = "rolled@test.com", .name = "Rolled"});
            ctx.save_changes();
            throw std::runtime_error{"force rollback"};
        });
    }
    catch(const std::exception&)
    {
        rolled_back = true;
    }

    require(rolled_back, "transaction propagated exception");
    require(ctx.db().table<rc_user>().count() == 1, "outer rollback undid savepoint");

    return status::passed;
}

status run_state_checks()
{
    {
        entityfy::context ctx{"sqlite://:memory:"};
        ctx.migrate<rc_user>();

        auto users = ctx.set<rc_user>();
        auto& value = users.add(rc_user{.email = "state@test.com", .name = "Initial"});
        ctx.save_changes();

        require(users.state(value) == entityfy::entity_state::unchanged, "state is unchanged after save");

        const std::int64_t value_id = value.id;
        value.name = "Changed";
        require(users.state(value) == entityfy::entity_state::modified, "modified state detected from snapshot");
        users.detach(value);

        const auto saved = ctx.save_changes();
        require(saved.updated == 0, "detached entity is not updated");

        const auto rows = ctx.db().table<rc_user>().find_many();
        require(rows.size() == 1 && rows.front().name == "Initial", "detached change was not persisted");

        auto* reloaded = users.find(value_id);
        require(reloaded != nullptr && reloaded->name == "Initial", "find reloads a detached entity");
    }

    const char* path = "rc_state.db";
    std::filesystem::remove(path);

    {
        entityfy::context writer{"sqlite://rc_state.db"};
        writer.migrate<rc_user>();

        auto writer_set = writer.set<rc_user>();
        auto& created = writer_set.add(rc_user{.email = "refresh@test.com", .name = "Before"});
        writer.save_changes();

        entityfy::context reader{"sqlite://rc_state.db"};
        auto reader_set = reader.set<rc_user>();
        auto* stale = reader_set.find(created.id);
        require(stale != nullptr, "reader loads the entity");

        created.name = "After";
        writer.save_changes();

        reader_set.refresh(*stale);
        require(stale->name == "After" && stale->version == 1, "refresh reloads entity and version");

        const auto saved = reader.save_changes();
        require(saved.updated == 0, "refreshed entity produces no update");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_retry_checks()
{
    const char* path = "rc_retry.db";
    std::filesystem::remove(path);

    {
        entityfy::context setup{"sqlite://rc_retry.db"};
        setup.migrate<rc_user>();
        setup.migrate<rc_post>();

        auto setup_users = setup.set<rc_user>();
        auto& user = setup_users.add(rc_user{.email = "retry@test.com", .name = "Retry"});
        setup.save_changes();

        auto setup_posts = setup.set<rc_post>();
        auto& post = setup_posts.add(rc_post{.user_id = user.id, .title = "initial", .score = 1});
        setup.save_changes();

        entityfy::context holder{"sqlite://rc_retry.db"};
        entityfy::context worker{"sqlite://rc_retry.db"};

        worker.execute("PRAGMA busy_timeout = 0");
        worker.set_retry_policy(entityfy::retry_policy{
            .attempts = 60,
            .delay = std::chrono::milliseconds(25),
        });

        auto holder_posts = holder.set<rc_post>();
        auto* held = holder_posts.find(post.id);
        auto worker_posts = worker.set<rc_post>();
        auto* stale = worker_posts.find(post.id);
        require(held != nullptr && stale != nullptr, "retry contexts load the post");

        std::atomic<bool> locked{false};
        std::thread holder_thread([&] {
            holder.transaction([&] {
                held->title = "holder";
                holder.save_changes();
                locked.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            });
        });

        while(!locked.load())
        {
            std::this_thread::yield();
        }

        stale->title = "worker";
        const auto saved = worker.save_changes();
        holder_thread.join();

        require(saved.updated == 1, "retry eventually applies the update");
        require(saved.attempts > 1, "retry actually occurred");

        const auto rows = setup.db().table<rc_post>().find_many();
        require(rows.size() == 1 && rows.front().title == "worker", "retry leaves the last writer");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_batch_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();
    ctx.migrate<rc_tag>();

    auto users = ctx.set<rc_user>();
    auto& first = users.add(rc_user{.email = "swap@test.com", .name = "First"});
    ctx.save_changes();
    const std::int64_t first_id = first.id;

    users.remove(first);
    auto& second = users.add(rc_user{.email = "swap@test.com", .name = "Second"});

    const auto swapped = ctx.save_changes();
    require(swapped.removed == 1 && swapped.inserted == 1, "batch applies delete before insert");

    const auto user_rows = ctx.db().table<rc_user>().find_many();
    require(user_rows.size() == 1 && user_rows.front().email == "swap@test.com" &&
                user_rows.front().id != first_id,
            "unique value is reused after delete");

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "one"});
    tags.add(rc_tag{.id = 2, .name = "two"});
    tags.add(rc_tag{.id = 3, .name = "three"});

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    const auto inserted = ctx.save_changes();
    require(inserted.inserted == 3, "explicit-key entities inserted");

    const auto insert_statement = std::ranges::find_if(statements, [](const std::string& sql) {
        return sql.find("INSERT INTO \"rc_tags\"") != std::string::npos;
    });
    require(insert_statement != statements.end(), "batched insert is logged");
    require(occurrences(*insert_statement, "), (") == 2, "three rows are batched in one statement");
    require(insert_statement->find("RETURNING") == std::string::npos, "explicit-key batch has no RETURNING");

    require(ctx.db().table<rc_tag>().count() == 3, "batched rows exist");

    entityfy::context chunked{"sqlite://:memory:"};
    chunked.migrate<rc_tag>();
    chunked.set_batch_parameter_limit(6); // 3 columns -> 2 rows per chunk

    auto chunked_tags = chunked.set<rc_tag>();
    chunked_tags.add(rc_tag{.id = 10, .name = "ten"});
    chunked_tags.add(rc_tag{.id = 11, .name = "eleven"});
    chunked_tags.add(rc_tag{.id = 12, .name = "twelve"});

    std::vector<std::string> chunk_statements;
    chunked.set_logger([&](std::string_view sql) { chunk_statements.emplace_back(sql); });

    const auto chunked_saved = chunked.save_changes();
    require(chunked_saved.inserted == 3, "chunked batch inserts all rows");

    const auto insert_count = std::ranges::count_if(chunk_statements, [](const std::string& sql) {
        return sql.find("INSERT INTO \"rc_tags\"") != std::string::npos;
    });
    require(insert_count == 2, "batch is chunked by parameter limit");
    require(chunked.db().table<rc_tag>().count() == 3, "chunked rows exist");

    return status::passed;
}

status run_dependency_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_parent>();
    ctx.migrate<rc_child>();

    auto children = ctx.set<rc_child>();
    auto& child = children.add(rc_child{.id = 10, .parent_id = 1, .label = "child"});

    auto parents = ctx.set<rc_parent>();
    auto& parent = parents.add(rc_parent{.id = 1, .name = "parent"});

    const auto saved = ctx.save_changes();
    require(saved.inserted == 2, "dependency order inserts both rows");
    require(ctx.db().table<rc_parent>().count() == 1, "parent row exists");
    require(ctx.db().table<rc_child>().count() == 1, "child row exists");

    children.remove(child);
    parents.remove(parent);

    const auto deleted = ctx.save_changes();
    require(deleted.removed == 2, "dependency order deletes both rows");
    require(ctx.db().table<rc_parent>().count() == 0, "parent row removed");
    require(ctx.db().table<rc_child>().count() == 0, "child row removed");

    return status::passed;
}

status run_postgres_checks()
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        if(postgres_required())
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_PG=1 but ENTITYFY_PG_URI is not set"};
        }

        std::cout << "postgres skipped: ENTITYFY_PG_URI is not set\n";
        return status::skipped;
    }

    entityfy::context ctx{uri};
    ctx.execute("SET client_min_messages TO WARNING");
    ctx.execute("DROP TABLE IF EXISTS \"rc_children\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_parents\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_tags\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_comments\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_posts\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_users\" CASCADE");
    ctx.migrate<rc_user>();
    ctx.migrate<rc_post>();
    ctx.migrate<rc_comment>();
    ctx.migrate<rc_tag>();
    ctx.migrate<rc_parent>();
    ctx.migrate<rc_child>();

    auto users = ctx.set<rc_user>();
    auto& ada = users.add(rc_user{.email = "ada@pg.test", .name = "Ada"});
    auto& grace = users.add(rc_user{.email = "grace@pg.test", .name = "Grace"});

    auto saved = ctx.save_changes();
    require(saved.inserted == 2, "pg insert count");

    require(users.query().sum(&rc_user::id) == std::optional<std::int64_t>{3}, "pg aggregate sum");
    require(users.query().avg(&rc_user::id) == std::optional<double>{1.5}, "pg aggregate avg");
    require(users.query().min(&rc_user::id) == std::optional<std::int64_t>{1}, "pg aggregate min");
    require(users.query().max(&rc_user::id) == std::optional<std::int64_t>{2}, "pg aggregate max");

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    const auto grouped = ctx.query(
        "SELECT \"name\", COUNT(*) AS \"n\" FROM \"rc_users\" GROUP BY \"name\" ORDER BY \"name\"");
    require(grouped.size() == 2, "pg raw group by row count");
    require(std::get<std::string>(grouped[0][0]) == "Ada", "pg raw group by name");
    require(as_integer(grouped[0][1]) == 1, "pg raw group by count");

    const auto filtered = ctx.query(
        "SELECT \"email\" FROM \"rc_users\" WHERE \"name\" = ?",
        {entityfy::to_sql_value(std::string{"Grace"})});
    require(filtered.size() == 1 && std::get<std::string>(filtered[0][0]) == "grace@pg.test",
            "pg raw query with binds");

    const auto affected = ctx.execute(
        "UPDATE \"rc_users\" SET \"name\" = \"name\" WHERE \"id\" = ?",
        {entityfy::to_sql_value(ada.id)});
    require(affected == 1, "pg execute with binds");

    statements.clear();
    ada.name = "Ada Lovelace";
    saved = ctx.save_changes();

    require(saved.updated == 1, "pg single update tracked");
    require(statements.size() == 1, "pg single update statement");
    require(statements.front().find("\"name\" = ?") != std::string::npos, "pg changed column is in update");
    require(statements.front().find("\"email\" = ?") == std::string::npos, "pg unchanged column is not in update");

    auto* fetched = users.find(ada.id);
    require(fetched != nullptr && fetched->name == "Ada Lovelace", "pg database updated");

    auto posts = ctx.set<rc_post>();
    auto& first_post = posts.add(rc_post{.user_id = ada.id, .title = "first", .score = 10});
    auto& second_post = posts.add(rc_post{.user_id = ada.id, .title = "second", .score = 20});
    auto& third_post = posts.add(rc_post{.user_id = grace.id, .title = "third", .score = 5});

    const auto posts_saved = ctx.save_changes();
    require(posts_saved.inserted == 3, "pg posts inserted");

    const auto selected = users.query()
                              .where(entityfy::where(&rc_user::email).ends_with("@pg.test"))
                              .order_by(&rc_user::name)
                              .take(1)
                              .to_list();
    require(selected.size() == 1 && selected.front() == &ada, "pg query filter/order/take");
    require(users.query().count() == 2, "pg query count");

    const auto joined = users.join<rc_post>(&rc_user::id, &rc_post::user_id);
    require(joined.size() == 3, "pg join row count");

    const auto counts = posts.query().count_by(&rc_post::user_id);
    require(counts.size() == 2 && counts[0].first == ada.id && counts[0].second == 2 &&
                counts[1].first == grace.id && counts[1].second == 1,
            "pg count_by");

    const auto pg_grouped = posts.query().group_count(&rc_post::user_id, &rc_post::score);
    require(pg_grouped.size() == 3, "pg multi-key group count");

    const auto pg_having = posts.query().group_count_having(
        entityfy::condition{.sql = "COUNT(*) >= 2"}, &rc_post::user_id);
    require(pg_having.size() == 1 && pg_having.begin()->second == 2, "pg grouped count with having");

    const auto sums = posts.query().sum_by(&rc_post::user_id, &rc_post::score);
    require(sums.size() == 2 && sums[0].second == 30 && sums[1].second == 5, "pg sum_by");

    const auto averages = posts.query().avg_by(&rc_post::user_id, &rc_post::score);
    require(averages.size() == 2 && averages[0].second == 15.0 && averages[1].second == 5.0, "pg avg_by");

    const auto minimums = posts.query().min_by(&rc_post::user_id, &rc_post::score);
    require(minimums.size() == 2 && minimums[0].second == 10 && minimums[1].second == 5, "pg min_by");

    const auto maximums = posts.query().max_by(&rc_post::user_id, &rc_post::score);
    require(maximums.size() == 2 && maximums[0].second == 20 && maximums[1].second == 5, "pg max_by");

    const auto included = users.include<rc_post>({&ada, &grace}, &rc_post::user_id);
    require(included.size() == 2 && included[0].second.size() == 2 && included[1].second.size() == 1,
            "pg include children");
    require(ada.posts.size() == 2 && grace.posts.size() == 1, "pg include fills navigation collections");
    require(ada.posts[0] == included[0].second[0], "pg navigation points at tracked child entities");

    const auto with_parents =
        posts.include_parent<rc_user>({&first_post, &second_post, &third_post}, &rc_post::user_id);
    require(with_parents.size() == 3 && with_parents[0].second == &ada && with_parents[2].second == &grace,
            "pg include_parent mapping");

    const auto projected = users.query().order_by(&rc_user::name).select(&rc_user::id, &rc_user::name);
    require(projected.size() == 2 && std::get<0>(projected[0]) == ada.id &&
                std::get<1>(projected[0]) == "Ada Lovelace",
            "pg projection values");

    const auto raw = users.from_sql(
        "SELECT \"id\", \"email\", \"name\", \"version\" FROM \"rc_users\" WHERE \"email\" = 'ada@pg.test'");
    require(raw.size() == 1 && raw.front() == &ada, "pg raw sql materialization");

    entityfy::context concurrent{uri};
    auto concurrent_users = concurrent.set<rc_user>();
    auto* stale = concurrent_users.find(ada.id);
    require(stale != nullptr, "pg concurrent context loads entity");

    ada.name = "Ada Winner";
    ctx.save_changes();

    stale->name = "Ada Loser";

    bool conflict = false;
    try
    {
        concurrent.save_changes();
    }
    catch(const std::exception&)
    {
        conflict = true;
    }

    require(conflict, "pg optimistic concurrency conflict");

    users.remove(*fetched);
    saved = ctx.save_changes();
    require(saved.removed == 1, "pg delete tracked");
    require(ctx.db().table<rc_user>().count() == 1, "pg row deleted");
    require(ctx.db().table<rc_post>().count() == 1, "pg delete cascade removed posts");

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "one"});
    tags.add(rc_tag{.id = 2, .name = "two"});
    tags.add(rc_tag{.id = 3, .name = "three"});

    statements.clear();
    const auto tag_inserted = ctx.save_changes();
    require(tag_inserted.inserted == 3, "pg explicit-key entities inserted");

    const auto tag_insert = std::ranges::find_if(statements, [](const std::string& sql) {
        return sql.find("INSERT INTO \"rc_tags\"") != std::string::npos;
    });
    require(tag_insert != statements.end() && occurrences(*tag_insert, "), (") == 2,
            "pg three rows batched in one statement");
    require(ctx.db().table<rc_tag>().count() == 3, "pg batched rows exist");

    const auto pg_json_rows = ctx.query("SELECT \"payload\" FROM \"rc_tags\" WHERE \"id\" = 1");
    require(pg_json_rows.size() == 1 && std::get<std::string>(pg_json_rows.front().front()) == "{}",
            "pg json column round trip from database");

    ctx.transaction([&] {
        users.add(rc_user{.email = "tx@pg.test", .name = "Tx"});
        ctx.save_changes();
    });
    require(ctx.db().table<rc_user>().count() == 2, "pg transaction commit persists");

    auto children = ctx.set<rc_child>();
    auto& child = children.add(rc_child{.id = 10, .parent_id = 1, .label = "child"});

    auto parents = ctx.set<rc_parent>();
    auto& parent = parents.add(rc_parent{.id = 1, .name = "parent"});

    const auto dependency_saved = ctx.save_changes();
    require(dependency_saved.inserted == 2, "pg dependency order inserts both rows");
    require(ctx.db().table<rc_parent>().count() == 1, "pg parent row exists");
    require(ctx.db().table<rc_child>().count() == 1, "pg child row exists");

    children.remove(child);
    parents.remove(parent);

    const auto dependency_deleted = ctx.save_changes();
    require(dependency_deleted.removed == 2, "pg dependency order deletes both rows");
    require(ctx.db().table<rc_parent>().count() == 0, "pg parent row removed");
    require(ctx.db().table<rc_child>().count() == 0, "pg child row removed");

    ctx.execute("DROP TABLE IF EXISTS \"rc_children\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_parents\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_tags\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_comments\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_posts\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_users\" CASCADE");

    return status::passed;
}

status run_pool_checks()
{
    entityfy::context_pool pool{"sqlite://:memory:", 1};
    const entityfy::context* first_context = nullptr;

    {
        auto lease = pool.acquire();
        first_context = lease.get();
        require(pool.created_count() == 1, "pool creates first context");
        require(pool.idle_count() == 0, "pool has no idle context while leased");

        lease->migrate<rc_tag>();
        auto tags = lease->set<rc_tag>();
        tags.add(rc_tag{.id = 1, .name = "pooled"});
        const auto saved = lease->save_changes();
        require(saved.inserted == 1, "pool context saves");
    }

    require(pool.idle_count() == 1, "pool gets context back");

    {
        auto lease = pool.acquire();
        require(lease.get() == first_context, "pool reuses context instance");
        require(lease->set<rc_tag>().size() == 0, "pooled context clears tracking on release");
        require(lease->db().table<rc_tag>().count() == 1, "pooled context keeps database state");
    }

    entityfy::context_pool broken{"bad://uri", 1};

    for(int attempt = 0; attempt < 2; ++attempt)
    {
        bool threw = false;

        try
        {
            auto lease = broken.acquire();
            (void)lease;
        }
        catch(const std::exception&)
        {
            threw = true;
        }

        require(threw, "pool propagates context construction failure");
    }

    require(broken.created_count() == 0, "failed pool construction is not counted");

    return status::passed;
}

status run_async_checks()
{
    entityfy::context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_tag>();

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "async"});

    auto pending = ctx.save_changes_async();
    const auto result = pending.get();

    require(result.inserted == 1, "async save inserts the pending row");
    require(ctx.db().table<rc_tag>().count() == 1, "async save persists the row");

    tags.add(rc_tag{.id = 2, .name = "second"});
    const auto second = ctx.save_changes_async().get();
    require(second.inserted == 1, "second async save inserts another row");
    require(ctx.db().table<rc_tag>().count() == 2, "both async saves are visible");

    return status::passed;
}

} // namespace

int main()
{
    std::cout << std::unitbuf;

    struct entry
    {
        const char* name;
        status (*run)();
    };

    const entry entries[] = {
        {"sqlite", run_sqlite_checks},
        {"rollback", run_rollback_checks},
        {"concurrency", run_concurrency_checks},
        {"multi_conflict", run_multi_conflict_checks},
        {"tracking_edges", run_tracking_edge_checks},
        {"transaction", run_transaction_checks},
        {"state", run_state_checks},
        {"retry", run_retry_checks},
        {"batch", run_batch_checks},
        {"dependency", run_dependency_checks},
        {"pool", run_pool_checks},
        {"async", run_async_checks},
        {"postgres", run_postgres_checks},
    };

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    for(const auto& current: entries)
    {
        try
        {
            const auto result = current.run();
            if(result == status::skipped)
            {
                std::cout << "[skip] " << current.name << "\n";
                ++skipped;
            }
            else
            {
                std::cout << "[pass] " << current.name << "\n";
                ++passed;
            }
        }
        catch(const std::exception& error)
        {
            std::cout << "[fail] " << current.name << ": " << error.what() << "\n";
            ++failed;
        }
    }

    std::cout << (failed == 0 ? "all tests passed" : "tests failed") << " (" << passed << " passed, " << skipped
              << " skipped, " << failed << " failed)\n";

    return failed == 0 ? 0 : 1;
}
