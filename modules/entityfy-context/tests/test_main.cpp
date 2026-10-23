#include <entityfy/concurrency.hpp>
#include <entityfy/db_context.hpp>
#include <entityfy/db_context_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
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

// Column aliases: every statement the context builds must use the name from
// [[= entityfy::column{"..."}]], not the C++ identifier.
struct [[= entityfy::table{"rc_alias_auto"}]] rc_alias_auto
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::column{"display_name"}, = entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::column{"score_value"}]]
    std::int64_t score = 0;
};

struct [[= entityfy::table{"rc_alias_key"}]] rc_alias_key
{
    [[= entityfy::primary_key, = entityfy::column{"alias_id"}]]
    std::int64_t id = 0;

    [[= entityfy::column{"body_text"}, = entityfy::not_null, = entityfy::varchar{200}]]
    std::string body;
};

struct [[= entityfy::table{"rc_alias_version"}]] rc_alias_version
{
    [[= entityfy::primary_key, = entityfy::column{"alias_id"}]]
    std::int64_t id = 0;

    [[= entityfy::column{"body_text"}, = entityfy::not_null, = entityfy::varchar{120}]]
    std::string body;

    [[= entityfy::column{"row_version"}]]
    std::int64_t version = 0;
};

struct [[= entityfy::table{"rc_token_model"}]] rc_token_model
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null]]
    std::string body;

    [[= entityfy::concurrency_token, = entityfy::not_null]]
    std::int64_t revision = 0;
};

// PostgreSQL system column as the concurrency token: the model has no
// application column for it.
struct [[= entityfy::table{"rc_xmin_rows"}]] rc_xmin_row
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null]]
    std::string body;

    [[= entityfy::xmin]]
    std::uint32_t revision = 0;
};

// Composite and string primary keys: the context identity map is keyed by
// row_key, so the tracker never assumes a single integral `id`.
struct [[= entityfy::table{"ck_people"}]] ck_person
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{32}]]
    std::string region;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::concurrency_token, = entityfy::not_null]]
    std::int64_t revision = 0;
};

struct [[= entityfy::table{"ck_slugs"}]] ck_slug
{
    [[= entityfy::primary_key, = entityfy::varchar{120}]]
    std::string slug;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

// Lazy loading: the collection is a plain [[= entityfy::ignore]] handle. The
// context binds it to the owner's key and filters on the foreign key declared
// by the "references" annotation, so no member pointer is needed. A reference
// handle is the same idea for the parent side: it loads the one row the
// foreign key points at.
struct lz_user;

struct [[= entityfy::table{"lz_posts"}]] lz_post
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"lz_users"}]]
    std::int64_t user_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::ignore]]
    entityfy::lazy<lz_user> author;
};

struct [[= entityfy::table{"lz_users"}]] lz_user
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::ignore]]
    entityfy::lazy<std::vector<lz_post>> posts;
};

static_assert(entityfy::detail::is_ignored_member(^^lz_user::posts), "posts must be ignored");
static_assert(entityfy::detail::is_ignored_member(^^lz_post::author), "author must be ignored");

// An include fills only the handle whose element type it loaded. A model
// without a matching handle resolves to an empty reflection, so the fill
// branch compiles away and the include keeps its old behavior.
static_assert(entityfy::detail::lazy_collection_member<lz_user, lz_post>() != std::meta::info{},
              "lz_user declares a lazy post collection");
static_assert(entityfy::detail::lazy_reference_member<lz_post, lz_user>() != std::meta::info{},
              "lz_post declares a lazy author reference");
static_assert(entityfy::detail::lazy_collection_member<lz_post, lz_user>() == std::meta::info{},
              "lz_post has no lazy collection to fill");
static_assert(entityfy::detail::lazy_reference_member<lz_user, lz_post>() == std::meta::info{},
              "lz_user has no lazy reference to fill");

// A nullable foreign key has no parent to load, so its handle caches nullptr
// without issuing a query.
struct [[= entityfy::table{"lz_notes"}]] lz_note
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::references{"lz_users"}]]
    std::optional<std::int64_t> user_id;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::ignore]]
    entityfy::lazy<lz_user> author;
};

// A one-column foreign key can only name part of this two-column key, so the
// relation does not point at the primary key and is not navigable: binding it
// is an explicit error instead of a silently wrong key.
struct [[= entityfy::table{"lz_pair_posts"}]] lz_pair_post
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"lz_pair_rows"}]]
    std::int64_t pair_id = 0;
};

struct [[= entityfy::table{"lz_pair_rows"}]] lz_pair_row
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key]]
    std::int64_t region = 0;

    [[= entityfy::ignore]]
    entityfy::lazy<std::vector<lz_pair_post>> children;
};

// The reference direction of the same shape: the parent's key has two columns
// and the one-column foreign key names only `id`, so it is not navigable.
struct [[= entityfy::table{"lz_pair_children"}]] lz_pair_child
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"lz_pair_rows"}]]
    std::int64_t pair_id = 0;

    [[= entityfy::ignore]]
    entityfy::lazy<lz_pair_row> pair;
};

// Composite navigation: the two columns of cn_parent's key are its identity,
// and the child names both of them in one grouped reference. include,
// include_parent and the lazy handles resolve the key as a single tuple, so a
// child that matches only one column never joins the wrong parent.
struct cn_child;

struct [[= entityfy::table{"cn_parents"}]] cn_parent
{
    [[= entityfy::primary_key]]
    std::int64_t tenant_id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{16}]]
    std::string code;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::ignore]]
    entityfy::lazy<std::vector<cn_child>> children;
};

struct [[= entityfy::table{"cn_children"}]] cn_child
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cn_parents", "tenant_id", "RESTRICT", "", 1}]]
    std::int64_t tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cn_parents", "code", "RESTRICT", "", 1}]]
    std::string parent_code;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string label;

    [[= entityfy::ignore]]
    entityfy::lazy<cn_parent> parent;
};

// EF Core style context: the sets are declared once, the framework derives
// everything else (table, columns, primary key, foreign keys) from reflection.
struct ef_context : entityfy::db_context
{
    using entityfy::db_context::db_context;

    entityfy::db_set<rc_user> users{*this};
    entityfy::db_set<rc_post> posts{*this};
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

// Shared by the SQLite entry point and the PostgreSQL checks, so both targets
// run the same composite-key assertions.
void run_composite_navigation_scenario(entityfy::db_context& ctx);

status run_ef_surface_checks()
{
    ef_context ctx{"sqlite://:memory:"};

    // The context collected both entity types through reflection when the
    // DbSet members were constructed.
    require(ctx.model().size() == 2, "model registry lists both entity types");

    bool user_table = false;
    bool post_table = false;
    bool post_points_at_users = false;

    for(const auto& model: ctx.model())
    {
        if(model.table == "rc_users")
        {
            user_table = true;
        }

        if(model.table == "rc_posts")
        {
            post_table = true;

            post_points_at_users = std::ranges::find(model.foreign_keys, std::string{"rc_users"}) !=
                                   model.foreign_keys.end();
        }
    }

    require(user_table && post_table, "reflection derived the table names");
    require(post_points_at_users, "reflection derived the foreign key graph");

    // Creates both tables in foreign key order without a manual migration list.
    ctx.migrate_all();

    auto& ada = ctx.users.add(rc_user{.email = "ada@ef.test", .name = "Ada"});
    auto& grace = ctx.users.add(rc_user{.email = "grace@ef.test", .name = "Grace"});

    const auto saved = ctx.save_changes();
    require(saved.inserted == 2, "save_changes inserts tracked rows");
    require(ada.id != 0 && grace.id != 0, "generated keys are written back");

    require(ctx.users.size() == 2, "size reports the tracked rows");
    require(ctx.users.find(ada.id) == &ada, "find returns the tracked instance");
    require(ctx.users.query().count() == 2, "query().count() reads the database count");

    auto* filtered = ctx.users.query()
                         .where(entityfy::where(&rc_user::email).ends_with("@ef.test"))
                         .order_by(&rc_user::name)
                         .first();
    require(filtered == &ada, "query().where/order_by/first");

    ctx.users.remove(grace);
    const auto removed = ctx.save_changes();
    require(removed.removed == 1, "remove marks the entity for deletion");
    require(ctx.users.query().count() == 1, "delete reached the database");

    ctx.posts.add(rc_post{.user_id = ada.id, .title = "ef", .score = 7});
    ctx.posts.add(rc_post{.user_id = ada.id, .title = "ef2", .score = 9});
    ctx.save_changes();

    // The foreign key is resolved from the "references" annotation, so the
    // caller never passes a member pointer.
    const auto included = ctx.users.include<rc_post>({&ada});
    require(included.size() == 1, "include returns one entry per parent");
    require(included[0].second.size() == 2, "include loads the children through the annotation");
    require(ada.posts.size() == 2, "include fills the navigation collection");

    const auto parents = ctx.posts.include_parent<rc_user>({included[0].second[0]});
    require(parents.size() == 1 && parents[0].second == &ada, "include_parent resolves the parent");

    ctx.clear();
    require(ctx.users.size() == 0, "clear drops the tracked entities");

    return status::passed;
}

status run_sqlite_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
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

    auto rows = ctx.database().table<rc_user>().find_many();
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

    const auto included = users.include<rc_post>({&ada, &grace});
    require(included.size() == 2, "include parent count");
    require(included[0].first == &ada && included[0].second.size() == 2, "include children of ada");
    require(included[1].first == &grace && included[1].second.size() == 1, "include children of grace");
    require(ada.posts.size() == 2 && grace.posts.size() == 1, "include fills navigation collections");
    require(ada.posts[0] == included[0].second[0], "navigation points at tracked child entities");

    const auto with_parents = posts.include_parent<rc_user>({&first_post, &second_post, &third_post});
    require(with_parents.size() == 3, "include_parent count");
    require(with_parents[0].second == &ada && with_parents[2].second == &grace, "include_parent mapping");

    rc_post unsaved_child;
    unsaved_child.title = "unsaved";
    const auto unsaved_parents = posts.include_parent<rc_user>({&unsaved_child});
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
    require(ctx.database().table<rc_user>().count() == 1, "row deleted from database");
    require(ctx.database().table<rc_post>().count() == 1, "delete cascade removed posts");

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
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();
    auto& first = users.add(rc_user{.email = "first@test.com", .name = "first"});
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

    auto rows = ctx.database().table<rc_user>().find_many();
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
        entityfy::db_context first_ctx{"sqlite://rc_concurrency.db"};
        first_ctx.migrate<rc_user>();

        auto first_users = first_ctx.set<rc_user>();
        auto& created = first_users.add(rc_user{.email = "concurrent@test.com", .name = "Initial"});
        first_ctx.save_changes();

        entityfy::db_context second_ctx{"sqlite://rc_concurrency.db"};
        auto second_users = second_ctx.set<rc_user>();

        auto* first_view = first_users.find(created.id);
        auto* second_view = second_users.find(created.id);
        require(first_view != nullptr && second_view != nullptr, "both contexts load entity");

        first_view->name = "first";
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

        const auto rows = first_ctx.database().table<rc_user>().find_many();
        require(rows.size() == 1 && rows.front().name == "first", "winner value kept");
        require(rows.front().version == 1, "version incremented once");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_concurrency_resolution_checks()
{
    const char* path = "rc_concurrency_resolution.db";
    std::filesystem::remove(path);

    {
        entityfy::db_context writer{"sqlite://rc_concurrency_resolution.db"};
        writer.migrate<rc_user>();

        auto writer_set = writer.set<rc_user>();
        auto& created = writer_set.add(rc_user{.email = "resolve@test.com", .name = "Initial"});
        writer.save_changes();
        const auto id = created.id;

        entityfy::db_context stale{"sqlite://rc_concurrency_resolution.db"};
        auto stale_set = stale.set<rc_user>();
        auto* stale_row = stale_set.find(id);
        require(stale_row != nullptr, "stale context loads the entity");
        require(stale_row->version == 0, "stale context sees version 0");

        auto* winner = writer_set.find(id);
        require(winner != nullptr, "writer tracks the entity");
        winner->name = "Winner";
        writer.save_changes();

        stale_row->name = "Loser";

        bool conflict = false;
        try
        {
            stale.save_changes();
        }
        catch(const entityfy::concurrency_error& error)
        {
            conflict = true;
            require(error.entries().size() == 1, "one conflicting entry");

            const auto& entry = error.entries().front();
            require(entry.table == "rc_users", "entry names the table");
            require(entry.key == entityfy::row_key{entityfy::sql_value{id}}, "entry carries the key");
            require(entry.token_column == "version", "entry names the token column");
            require(as_integer(entry.original_token) == 0, "entry carries the stale token");
            require(entry.row_exists(), "entry carries the database row");

            const auto name = std::find(entry.columns.begin(), entry.columns.end(), std::string{"name"});
            require(name != entry.columns.end(), "entry columns include name");
            const auto position = static_cast<std::size_t>(std::distance(entry.columns.begin(), name));
            require(std::get<std::string>(entry.database_values[position]) == "Winner",
                    "entry exposes the winner's value");
        }

        require(conflict, "structured concurrency error raised");

        const auto database_row = stale_set.database_values(*stale_row);
        require(database_row.has_value(), "database_values returns the current row");
        require(database_row->name == "Winner" && database_row->version == 1, "database row is current");

        stale_set.accept_changes(*stale_row, *database_row);
        stale_row->name = "Resolved";
        require(stale.save_changes().updated == 1, "client wins save succeeds");

        auto rows = writer.database().table<rc_user>().find_many();
        require(rows.size() == 1 && rows.front().name == "Resolved", "client wins value persisted");
        require(rows.front().version == 2, "token incremented after resolution");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_attach_and_delete_conflict_checks()
{
    const char* path = "rc_attach_conflict.db";
    std::filesystem::remove(path);

    {
        entityfy::db_context writer{"sqlite://rc_attach_conflict.db"};
        writer.migrate<rc_user>();

        auto set = writer.set<rc_user>();
        auto& created = set.add(rc_user{.email = "attach@test.com", .name = "Initial"});
        writer.save_changes();
        const auto id = created.id;

        // A detached copy taken before the update keeps the stale token.
        auto stale_copy = *writer.database().find<rc_user>(id);

        auto* tracked = set.find(id);
        tracked->name = "Winner";
        writer.save_changes();

        entityfy::db_context detached{"sqlite://rc_attach_conflict.db"};
        auto detached_set = detached.set<rc_user>();
        auto attached = stale_copy;
        attached.name = "Attached";
        attached.version = 99; // the current value must not replace the snapshot
        detached_set.attach(attached, stale_copy);

        bool conflict = false;
        try
        {
            detached.save_changes();
        }
        catch(const entityfy::concurrency_error& error)
        {
            conflict = true;
            require(error.entries().size() == 1, "attach conflict reports one entry");
            require(as_integer(error.entries().front().original_token) == 0,
                    "attach uses the provided original token");
        }

        require(conflict, "stale attached entity conflicts");

        // Delete conflict: the removed row carries the token too.
        auto* to_remove = set.find(id);
        require(to_remove != nullptr, "writer tracks the entity for removal");
        writer.save_changes();
        to_remove->name = "Before delete";
        writer.save_changes();

        entityfy::db_context stale_delete{"sqlite://rc_attach_conflict.db"};
        auto stale_remove_set = stale_delete.set<rc_user>();
        auto* stale_remove = stale_remove_set.find(id);
        require(stale_remove != nullptr, "stale context loads the deletable entity");

        to_remove->name = "Concurrent update";
        writer.save_changes();

        stale_remove_set.remove(*stale_remove);

        bool delete_conflict = false;
        try
        {
            stale_delete.save_changes();
        }
        catch(const entityfy::concurrency_error& error)
        {
            delete_conflict = true;
            require(error.entries().size() == 1, "delete conflict reports one entry");
            require(error.entries().front().row_exists(), "delete conflict carries the current row");
        }

        require(delete_conflict, "stale delete conflicts");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_token_annotation_checks()
{
    std::filesystem::remove("rc_token_model.db");

    {
        entityfy::db_context writer{"sqlite://rc_token_model.db"};
        writer.migrate<rc_token_model>();
        auto writer_set = writer.set<rc_token_model>();
        auto& row = writer_set.add(rc_token_model{.body = "v0"});
        writer.save_changes();

        entityfy::db_context stale_ctx{"sqlite://rc_token_model.db"};
        auto stale_set = stale_ctx.set<rc_token_model>();
        auto* stale_row = stale_set.find(row.id);
        require(stale_row != nullptr, "stale context loads annotated token entity");

        auto* winner = writer_set.find(row.id);
        winner->body = "winner";
        writer.save_changes();

        stale_row->body = "loser";
        bool conflict = false;
        try
        {
            stale_ctx.save_changes();
        }
        catch(const entityfy::concurrency_error& error)
        {
            conflict = true;
            require(error.entries().front().token_column == "revision", "annotation selects the token column");
        }

        require(conflict, "annotated token detects the conflict");
    }

    std::filesystem::remove("rc_token_model.db");
    return status::passed;
}

status run_multi_conflict_checks()
{
    const char* path = "rc_multi_conflict.db";
    std::filesystem::remove(path);

    {
        entityfy::db_context writer{"sqlite://rc_multi_conflict.db"};
        writer.migrate<rc_user>();

        auto writer_set = writer.set<rc_user>();
        auto& first = writer_set.add(rc_user{.email = "a@multi.test", .name = "A"});
        auto& second = writer_set.add(rc_user{.email = "b@multi.test", .name = "B"});
        writer.save_changes();

        entityfy::db_context stale{"sqlite://rc_multi_conflict.db"};
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

        auto rows = writer.database().table<rc_user>().find_many();
        for(const auto& row: rows)
        {
            require(row.version == 0, "database version rolled back");
            require(row.email != "a@multi.test" || row.name == "A", "database email unchanged for second");
        }

        stale_second->email = "b2@multi.test";
        const auto retried = stale.save_changes();
        require(retried.updated == 2, "retry updates both entities");

        rows = writer.database().table<rc_user>().find_many();
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
    entityfy::db_context ctx{"sqlite://:memory:"};
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
    require(ctx.database().table<rc_user>().count() == 0, "no row inserted for rejected explicit id");

    users.remove(explicit_id);

    auto& added = users.add(rc_user{.email = "cancel@test.com", .name = "Cancel"});
    users.remove(added);

    const auto saved = ctx.save_changes();
    require(saved.inserted == 0 && saved.removed == 0, "cancelled add is not persisted");
    require(ctx.database().table<rc_user>().count() == 0, "cancelled add leaves database empty");

    return status::passed;
}

status run_transaction_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();

    ctx.transaction([&] {
        users.add(rc_user{.email = "committed@test.com", .name = "Committed"});
        ctx.save_changes();
    });

    require(ctx.database().table<rc_user>().count() == 1, "transaction commit persists");

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
    require(ctx.database().table<rc_user>().count() == 1, "outer rollback undid savepoint");

    return status::passed;
}

status run_state_checks()
{
    {
        entityfy::db_context ctx{"sqlite://:memory:"};
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

        const auto rows = ctx.database().table<rc_user>().find_many();
        require(rows.size() == 1 && rows.front().name == "Initial", "detached change was not persisted");

        auto* reloaded = users.find(value_id);
        require(reloaded != nullptr && reloaded->name == "Initial", "find reloads a detached entity");
    }

    const char* path = "rc_state.db";
    std::filesystem::remove(path);

    {
        entityfy::db_context writer{"sqlite://rc_state.db"};
        writer.migrate<rc_user>();

        auto writer_set = writer.set<rc_user>();
        auto& created = writer_set.add(rc_user{.email = "refresh@test.com", .name = "Before"});
        writer.save_changes();

        entityfy::db_context reader{"sqlite://rc_state.db"};
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
        entityfy::db_context setup{"sqlite://rc_retry.db"};
        setup.migrate<rc_user>();
        setup.migrate<rc_post>();

        auto setup_users = setup.set<rc_user>();
        auto& user = setup_users.add(rc_user{.email = "retry@test.com", .name = "Retry"});
        setup.save_changes();

        auto setup_posts = setup.set<rc_post>();
        auto& post = setup_posts.add(rc_post{.user_id = user.id, .title = "initial", .score = 1});
        setup.save_changes();

        entityfy::db_context holder{"sqlite://rc_retry.db"};
        entityfy::db_context worker{"sqlite://rc_retry.db"};

        worker.execute("PRAGMA busy_timeout = 0");
        worker.set_retry_policy(entityfy::retry_policy{
            .attempts = 60,
            .delay = std::chrono::milliseconds{25},
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

        const auto rows = setup.database().table<rc_post>().find_many();
        require(rows.size() == 1 && rows.front().title == "worker", "retry leaves the last writer");
    }

    std::filesystem::remove(path);
    return status::passed;
}

status run_column_alias_checks()
{
    const char* path = "sqlite://rc_alias.db";
    std::filesystem::remove("rc_alias.db");

    {
        entityfy::db_context ctx{path};
        ctx.migrate<rc_alias_auto>();
        ctx.migrate<rc_alias_key>();

        // Single row INSERT goes through the per entity statement path.
        auto aliases = ctx.set<rc_alias_auto>();
        auto& single = aliases.add(rc_alias_auto{.name = "aliased", .score = 7});
        const auto inserted = ctx.save_changes();
        require(inserted.inserted == 1, "aliased single insert did not insert one row");
        require(single.id != 0, "aliased single insert did not write back the generated key");

        // Explicit primary keys take the batched INSERT path.
        auto keys = ctx.set<rc_alias_key>();

        std::vector<std::string> statements;
        ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

        keys.add(rc_alias_key{.id = 1, .body = "one"});
        keys.add(rc_alias_key{.id = 2, .body = "two"});
        const auto batched = ctx.save_changes();
        require(batched.inserted == 2, "aliased batch insert did not insert two rows");

        const auto batch_statement = std::ranges::find_if(statements, [](const std::string& sql) {
            return sql.find("INSERT INTO \"rc_alias_key\"") != std::string::npos;
        });
        require(batch_statement != statements.end(), "aliased batch insert is not a batched statement");
        require(occurrences(*batch_statement, "), (") == 1, "aliased rows are not batched together");
        require(batch_statement->find("\"alias_id\"") != std::string::npos,
                "aliased batch insert ignores the aliased primary key");
        require(batch_statement->find("\"body_text\"") != std::string::npos,
                "aliased batch insert ignores the aliased column");
        require(batch_statement->find("\"body\"") == std::string::npos,
                "aliased batch insert leaks the C++ identifier");

        // UPDATE compares the snapshot and must name display_name in the SET list.
        single.name = "renamed";
        single.score = 9;
        const auto updated = ctx.save_changes();
        require(updated.updated == 1, "aliased update did not update one row");
    }

    // A fresh context can only see the rows if SELECT mapped the aliased
    // columns back onto the members.
    {
        entityfy::db_context verify{path};
        auto aliases = verify.set<rc_alias_auto>();
        const auto loaded = aliases.to_list();
        require(loaded.size() == 1, "aliased select lost the row");
        require(loaded.front()->name == "renamed", "aliased select did not map display_name");
        require(loaded.front()->score == 9, "aliased select did not map score_value");

        auto keys = verify.set<rc_alias_key>();
        const auto batches = keys.to_list();
        require(batches.size() == 2, "aliased batch insert rows are not readable");
        require(batches.front()->body == "one" && batches.back()->body == "two",
                "aliased batch insert wrote the wrong column");

        auto* found = aliases.find(1);
        require(found != nullptr && found->name == "renamed", "aliased find did not resolve the key");
    }

    std::filesystem::remove("rc_alias.db");

    return status::passed;
}

status run_column_alias_version_checks()
{
    const char* path = "sqlite://rc_alias_version.db";
    std::filesystem::remove("rc_alias_version.db");

    {
        entityfy::db_context writer{path};
        writer.migrate<rc_alias_version>();

        auto writer_set = writer.set<rc_alias_version>();
        writer_set.add(rc_alias_version{.id = 1, .body = "v0"});
        const auto inserted = writer.save_changes();
        require(inserted.inserted == 1, "aliased version model did not insert");

        // Loaded before the update, so this context holds version 0.
        entityfy::db_context stale{path};
        auto stale_set = stale.set<rc_alias_version>();
        auto* stale_row = stale_set.find(1);
        require(stale_row != nullptr && stale_row->version == 0, "stale context loads version 0");

        auto* fresh_row = writer_set.find(1);
        require(fresh_row != nullptr, "writer keeps the tracked row");
        fresh_row->body = "v1";
        const auto bumped = writer.save_changes();
        require(bumped.updated == 1, "aliased version update did not update one row");
        require(fresh_row->version == 1, "aliased version column was not incremented");

        // The UPDATE names row_version, so the stale snapshot must be rejected.
        stale_row->body = "v2";
        bool conflicted = false;
        try
        {
            stale.save_changes();
        }
        catch(const std::exception&)
        {
            conflicted = true;
        }

        require(conflicted, "aliased version column did not detect the stale write");

        auto rows = writer.database().table<rc_alias_version>().find_many();
        require(rows.size() == 1 && rows.front().body == "v1", "stale write overwrote the row");
    }

    std::filesystem::remove("rc_alias_version.db");

    return status::passed;
}

status run_manual_tracking_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();
    users.add(rc_user{.email = "a@manual.test", .name = "A"});
    users.add(rc_user{.email = "b@manual.test", .name = "B"});
    ctx.save_changes();

    require(ctx.auto_detect(), "auto detection is on by default");
    ctx.set_auto_detect(false);
    require(!ctx.auto_detect(), "auto detection can be turned off");

    auto* first = users.find(1);
    auto* second = users.find(2);
    require(first != nullptr && second != nullptr, "manual mode loads both rows");

    first->name = "A changed without a mark";
    second->name = "B marked";
    users.mark_modified(*second);

    require(users.state(*first) == entityfy::entity_state::unchanged,
            "manual mode reports an unmarked entity as unchanged");
    require(users.state(*second) == entityfy::entity_state::modified,
            "manual mode reports a marked entity as modified");

    const auto saved = ctx.save_changes();
    require(saved.updated == 1, "manual mode writes only the marked entity");

    const auto rows = ctx.database().table<rc_user>().find_many();
    std::string first_name;
    std::string second_name;
    for(const auto& row: rows)
    {
        if(row.email == "a@manual.test")
        {
            first_name = row.name;
        }
        else if(row.email == "b@manual.test")
        {
            second_name = row.name;
        }
    }

    require(first_name == "A", "the unmarked edit stayed in memory");
    require(second_name == "B marked", "the marked edit reached the database");

    ctx.set_auto_detect(true);
    const auto detected = ctx.save_changes();
    require(detected.updated == 1, "auto detection writes the pending edit");

    rc_user stranger{.email = "stranger@manual.test", .name = "X"};
    bool rejected = false;
    try
    {
        users.mark_modified(stranger);
    }
    catch(const std::exception&)
    {
        rejected = true;
    }

    require(rejected, "mark_modified rejects untracked entities");

    auto& fresh = users.add(rc_user{.email = "c@manual.test", .name = "C"});
    users.mark_modified(fresh);
    const auto inserted = ctx.save_changes();
    require(inserted.inserted == 1, "marking an added entity does not disturb the insert");

    return status::passed;
}

status run_no_tracking_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();

    auto users = ctx.set<rc_user>();
    for(int index = 1; index <= 3; ++index)
    {
        const auto suffix = std::to_string(index);
        users.add(rc_user{.email = "user" + suffix + "@test.com", .name = "User " + suffix});
    }
    ctx.save_changes();

    const auto tracked_before = users.size();
    require(tracked_before == 3, "tracked set holds the inserted rows");

    const auto filtered = users.as_no_tracking().where(entityfy::where(&rc_user::name).eq("User 2")).to_list();
    require(filtered.size() == 1 && filtered.front().email == "user2@test.com", "no-tracking filter reads the row");
    require(users.size() == tracked_before, "no-tracking filter did not grow the tracker");

    const auto every = users.as_no_tracking().order_by(&rc_user::name).to_list();
    static_assert(std::is_const_v<std::remove_reference_t<decltype(every[0])>>,
                  "no-tracking rows are const, so mutating one does not compile");
    require(every.size() == 3, "no-tracking to_list returns every row");
    require(every.front().name == "User 1" && every.back().name == "User 3", "no-tracking ordering");
    require(users.size() == tracked_before, "no-tracking to_list did not grow the tracker");

    const auto last = users.as_no_tracking()
                          .order_by(&rc_user::name, entityfy::sort_direction::descending)
                          .first();
    require(last.has_value() && last->name == "User 3", "no-tracking first honours ordering");

    require(users.as_no_tracking().count() == 3, "no-tracking count");

    // Documented caveat: the values belong to the caller, so changing them
    // compiles but never reaches the database.
    auto copy = users.as_no_tracking().where(entityfy::where(&rc_user::name).eq("User 1")).first();
    require(copy.has_value(), "no-tracking first returns a value");

    copy->name = "changed outside the tracker";
    const auto saved = ctx.save_changes();
    require(saved.updated == 0, "mutating a no-tracking value is not saved");
    require(copy->name == "changed outside the tracker", "the caller owns its copy");

    const auto rows = ctx.database().table<rc_user>().find_many();
    bool untouched = true;
    for(const auto& row: rows)
    {
        untouched = untouched && row.name.rfind("User ", 0) == 0;
    }
    require(untouched, "no-tracking mutation reached the database");

    // Eager loading on a no-tracking read: value pairs, no tracker entries.
    ctx.migrate<rc_post>();
    auto posts = ctx.set<rc_post>();

    const auto owner_id = every.front().id;
    const auto other_user = users.as_no_tracking().where(entityfy::where(&rc_user::name).eq("User 2")).first();
    require(other_user.has_value(), "no-tracking owner lookup");

    posts.add(rc_post{.user_id = owner_id, .title = "first", .score = 1});
    posts.add(rc_post{.user_id = owner_id, .title = "second", .score = 2});
    posts.add(rc_post{.user_id = other_user->id, .title = "third", .score = 3});
    ctx.save_changes();

    const auto with_posts = users.as_no_tracking().order_by(&rc_user::name).include<rc_post>().to_list();
    require(with_posts.size() == 3, "no-tracking include row count");
    require(with_posts[0].first.name == "User 1" && with_posts[0].second.size() == 2,
            "no-tracking include buckets children by key");
    require(with_posts[1].first.name == "User 2" && with_posts[1].second.size() == 1,
            "no-tracking include resolves each parent");
    require(with_posts[2].second.empty(), "no-tracking include leaves unrelated parents empty");
    require(users.size() == tracked_before, "no-tracking include did not grow the tracker");

    return status::passed;
}

status run_batch_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_user>();
    ctx.migrate<rc_tag>();

    auto users = ctx.set<rc_user>();
    auto& first = users.add(rc_user{.email = "swap@test.com", .name = "first"});
    ctx.save_changes();
    const std::int64_t first_id = first.id;

    users.remove(first);
    auto& second = users.add(rc_user{.email = "swap@test.com", .name = "Second"});
    (void)second;

    const auto swapped = ctx.save_changes();
    require(swapped.removed == 1 && swapped.inserted == 1, "batch applies delete before insert");

    const auto user_rows = ctx.database().table<rc_user>().find_many();
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

    require(ctx.database().table<rc_tag>().count() == 3, "batched rows exist");

    entityfy::db_context chunked{"sqlite://:memory:"};
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
    require(chunked.database().table<rc_tag>().count() == 3, "chunked rows exist");

    return status::passed;
}

status run_dependency_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_parent>();
    ctx.migrate<rc_child>();

    auto children = ctx.set<rc_child>();
    auto& child = children.add(rc_child{.id = 10, .parent_id = 1, .label = "child"});

    auto parents = ctx.set<rc_parent>();
    auto& parent = parents.add(rc_parent{.id = 1, .name = "parent"});

    const auto saved = ctx.save_changes();
    require(saved.inserted == 2, "dependency order inserts both rows");
    require(ctx.database().table<rc_parent>().count() == 1, "parent row exists");
    require(ctx.database().table<rc_child>().count() == 1, "child row exists");

    children.remove(child);
    parents.remove(parent);

    const auto deleted = ctx.save_changes();
    require(deleted.removed == 2, "dependency order deletes both rows");
    require(ctx.database().table<rc_parent>().count() == 0, "parent row removed");
    require(ctx.database().table<rc_child>().count() == 0, "child row removed");

    return status::passed;
}

status run_pool_checks()
{
    entityfy::db_context_pool pool{"sqlite://:memory:", 1};
    const entityfy::db_context* first_context = nullptr;

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
        require(lease->database().table<rc_tag>().count() == 1, "pooled context keeps database state");
    }

    entityfy::db_context_pool broken{"bad://uri", 1};

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

// Holds a save inside its first logged statement until release() is called, so
// a test can observe the context while a save is genuinely in flight.
struct save_hold
{
    void wait_until_entered()
    {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return entered; });
    }

    void release()
    {
        const std::lock_guard lock{mutex};
        released = true;
        cv.notify_all();
    }

    void operator()(std::string_view)
    {
        std::unique_lock lock{mutex};
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
    }

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    entered = false;
    bool                    released = false;
};

status run_async_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    ctx.migrate<rc_tag>();

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "async"});

    auto pending = ctx.save_changes_async();
    const auto result = pending.get();

    require(result.inserted == 1, "async save inserts the pending row");
    require(ctx.database().table<rc_tag>().count() == 1, "async save persists the row");

    tags.add(rc_tag{.id = 2, .name = "second"});
    const auto second = ctx.save_changes_async().get();
    require(second.inserted == 1, "second async save inserts another row");
    require(ctx.database().table<rc_tag>().count() == 2, "both async saves are visible");

    // Hold the worker inside the save, so the gate is observably taken.
    save_hold hold;
    ctx.set_logger([&hold](std::string_view sql) { hold(sql); });

    tags.add(rc_tag{.id = 3, .name = "gated"});
    auto held = ctx.save_changes_async();
    hold.wait_until_entered();

    bool sync_rejected = false;
    try
    {
        (void)ctx.save_changes();
    }
    catch(const std::logic_error&)
    {
        sync_rejected = true;
    }
    require(sync_rejected, "a sync save is rejected while an async save is in flight");

    bool async_rejected = false;
    try
    {
        auto ignored = ctx.save_changes_async();
        (void)ignored;
    }
    catch(const std::logic_error&)
    {
        async_rejected = true;
    }
    require(async_rejected, "a concurrent async save is rejected instead of queued");

    hold.release();

    const auto held_result = held.get();
    require(held_result.inserted == 1, "the held async save inserts its row");
    require(ctx.database().table<rc_tag>().count() == 3, "the held async save is visible");

    ctx.set_logger(nullptr);

    // Both entry points work again once the completed save released the gate.
    tags.add(rc_tag{.id = 4, .name = "after gate sync"});
    require(ctx.save_changes().inserted == 1, "a sync save works after the gate is free");

    tags.add(rc_tag{.id = 5, .name = "after gate async"});
    require(ctx.save_changes_async().get().inserted == 1, "an async save works after the gate is free");
    require(ctx.database().table<rc_tag>().count() == 5, "every save after the gate is visible");

    // A save that throws must release the gate too, not only a successful one.
    {
        entityfy::db_context failing{"sqlite://:memory:"};
        failing.migrate<rc_user>();

        auto users = failing.set<rc_user>();
        users.add(rc_user{.email = "duplicate@example.com", .name = "first"});
        auto& second_user = users.add(rc_user{.email = "duplicate@example.com", .name = "second"});

        auto failed = failing.save_changes_async();

        bool save_failed = false;
        try
        {
            (void)failed.get();
        }
        catch(const std::exception&)
        {
            save_failed = true;
        }
        require(save_failed, "an async save surfaces its error through the task");

        second_user.email = "unique@example.com";
        require(failing.save_changes().inserted == 2, "the gate is free after a failed async save");
    }

    return status::passed;
}

status run_async_drain_checks()
{
    struct destroying_context final : entityfy::db_context
    {
        using entityfy::db_context::db_context;

        ~destroying_context()
        {
            started.set_value();
        }

        std::promise<void> started;
    };

    auto ctx = std::make_unique<destroying_context>("sqlite://:memory:");
    ctx->migrate<rc_tag>();
    ctx->set<rc_tag>().add(rc_tag{.id = 1, .name = "drained"});

    save_hold hold;
    ctx->set_logger([&hold](std::string_view sql) { hold(sql); });

    auto pending = ctx->save_changes_async();
    hold.wait_until_entered();

    auto destruction_started = ctx->started.get_future();

    // Destruction begins while the worker is still blocked inside the save; the
    // executor must join it before the members the save uses are torn down.
    std::thread destroyer{[&ctx] { ctx.reset(); }};
    destruction_started.wait();

    hold.release();
    destroyer.join();

    require(ctx == nullptr, "the context is destroyed");

    const auto result = pending.get();
    require(result.inserted == 1, "destroying the context drains the in-flight save");

    return status::passed;
}

status run_composite_key_checks()
{
    const char* path = "rc_composite_key.db";
    std::filesystem::remove(path);

    {
        entityfy::db_context ctx{"sqlite://:memory:"};
        ctx.migrate<ck_person>();
        ctx.migrate<ck_slug>();

        std::vector<std::string> statements;
        ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

        auto people = ctx.set<ck_person>();

        auto& alpha = people.add(ck_person{.id = 1, .region = "eu", .name = "alpha"});
        auto& beta = people.add(ck_person{.id = 2, .region = "eu", .name = "beta"});
        (void)beta;

        bool duplicate = false;
        try
        {
            people.add(ck_person{.id = 1, .region = "eu", .name = "duplicate"});
        }
        catch(const std::runtime_error&)
        {
            duplicate = true;
        }

        require(duplicate, "a duplicate composite key is rejected by the tracker");

        const auto alpha_key = entityfy::row_key{entityfy::sql_value{std::int64_t{1}},
                                                 entityfy::sql_value{std::string{"eu"}}};

        require(people.find(alpha_key) == &alpha, "find(row_key) returns the tracked composite entity");
        require(people.find(alpha_key) == &alpha, "the identity map reuses the tracked instance");

        const auto inserted = ctx.save_changes();
        require(inserted.inserted == 2, "both composite rows are inserted");
        require(ctx.database().table<ck_person>().count() == 2, "composite rows reached the database");

        require(people.state(alpha) == entityfy::entity_state::unchanged, "a saved entity starts unchanged");

        // Both key parts are excluded from change detection: moving them in
        // place must not report a modification.
        const std::int64_t saved_id = alpha.id;
        const std::string saved_region = alpha.region;
        alpha.id = 7;
        alpha.region = "us";
        require(people.state(alpha) == entityfy::entity_state::unchanged,
                "state_of skips every primary key member");
        alpha.id = saved_id;
        alpha.region = saved_region;

        statements.clear();
        alpha.name = "alpha2";
        require(people.state(alpha) == entityfy::entity_state::modified, "a changed column is detected");

        const auto updated = ctx.save_changes();
        require(updated.updated == 1, "composite key update writes one row");

        const auto update_sql = std::ranges::find_if(statements, [](const std::string& sql) {
            return sql.starts_with("UPDATE \"ck_people\"");
        });
        require(update_sql != statements.end(), "the update statement was logged");
        require(update_sql->find("SET \"name\" = ?") != std::string::npos,
                "only the non-key column is in the SET list");
        require(update_sql->find("WHERE \"id\" = ? AND \"region\" = ?") != std::string::npos,
                "the WHERE clause binds every key part in order");

        require(people.find(entityfy::row_key{entityfy::sql_value{std::int64_t{1}},
                                              entityfy::sql_value{std::string{"us"}}}) == nullptr,
                "a different composite key is not the tracked entity");

        const auto database_row = people.database_values(alpha);
        require(database_row.has_value() && database_row->name == "alpha2",
                "database_values loads a composite row");

        statements.clear();
        people.remove(alpha);
        const auto removed = ctx.save_changes();
        require(removed.removed == 1, "composite key delete removes one row");

        const auto delete_sql = std::ranges::find_if(statements, [](const std::string& sql) {
            return sql.starts_with("DELETE FROM \"ck_people\"");
        });
        require(delete_sql != statements.end(), "the delete statement was logged");
        require(delete_sql->find("WHERE \"id\" = ? AND \"region\" = ?") != std::string::npos,
                "the DELETE predicate binds every key part in order");

        require(ctx.database().table<ck_person>().count() == 1, "one composite row remains");

        // An explicit zero key is a real key, not the generated "not assigned"
        // default.
        auto& zero = people.add(ck_person{.id = 0, .region = "zero", .name = "zero"});
        require(ctx.save_changes().inserted == 1, "an explicit zero key is inserted");
        require(people.find(entityfy::row_key{entityfy::sql_value{std::int64_t{0}},
                                              entityfy::sql_value{std::string{"zero"}}}) == &zero,
                "find returns the entity tracked under a zero key");

        // String primary key: the same tracker path, no integral member.
        auto slugs = ctx.set<ck_slug>();

        const auto slug_key = entityfy::row_key{entityfy::sql_value{std::string{"entityfy"}}};

        auto& slot = slugs.add(ck_slug{.slug = "entityfy", .name = "Entityfy"});
        require(ctx.save_changes().inserted == 1, "the string-key row is inserted");
        require(slugs.find(slug_key) == &slot, "find(row_key) returns the tracked string-key entity");

        slot.name = "Entityfy ORM";
        require(ctx.save_changes().updated == 1, "the string-key row is updated");

        auto* reloaded = slugs.find(std::string{"entityfy"});
        require(reloaded == &slot && reloaded->name == "Entityfy ORM",
                "the single-value find overload builds the one-part key");

        const auto empty_key = entityfy::row_key{entityfy::sql_value{std::string{}}};
        auto& empty = slugs.add(ck_slug{.slug = "", .name = "empty"});
        require(ctx.save_changes().inserted == 1, "an empty string key is a real key");
        require(slugs.find(empty_key) == &empty, "find returns the entity tracked under an empty key");

        slugs.remove(*reloaded);
        require(ctx.save_changes().removed == 1, "the string-key row is removed");
        require(slugs.find(slug_key) == nullptr, "a removed key is no longer tracked");
    }

    {
        const auto key = entityfy::row_key{entityfy::sql_value{std::int64_t{7}},
                                           entityfy::sql_value{std::string{"eu"}}};

        entityfy::db_context writer{"sqlite://rc_composite_key.db"};
        writer.migrate<ck_person>();

        auto writer_set = writer.set<ck_person>();
        auto& created = writer_set.add(ck_person{.id = 7, .region = "eu", .name = "before"});
        require(writer.save_changes().inserted == 1, "the concurrency row is inserted");
        require(created.revision == 0, "a new row starts at revision zero");

        entityfy::db_context stale{"sqlite://rc_composite_key.db"};
        auto stale_set = stale.set<ck_person>();
        auto* stale_row = stale_set.find(key);
        require(stale_row != nullptr, "the stale context loads the composite row");
        require(stale_row->revision == 0, "the stale context sees revision zero");

        auto* winner = writer_set.find(key);
        require(winner == &created, "the writer tracks the same instance by row_key");
        winner->name = "winner";
        require(writer.save_changes().updated == 1, "the writer updates the composite row");

        stale_row->name = "loser";

        bool conflict = false;
        try
        {
            stale.save_changes();
        }
        catch(const entityfy::concurrency_error& error)
        {
            conflict = true;
            require(error.entries().size() == 1, "one conflicting entry for the composite row");

            const auto& entry = error.entries().front();
            require(entry.key.size() == 2, "the concurrency entry carries both key parts");
            require(entry.key.to_string().find("7") != std::string::npos, "the entry names the id part");
            require(entry.key.to_string().find("eu") != std::string::npos, "the entry names the region part");
            require(entry.token_column == "revision", "the annotated token column is used");
            require(entry.row_exists(), "the entry carries the current database row");
        }

        require(conflict, "a stale composite save conflicts");

        entityfy::db_context attached_ctx{"sqlite://rc_composite_key.db"};
        auto attached_set = attached_ctx.set<ck_person>();

        ck_person detached{.id = 7, .region = "eu", .name = "detached", .revision = 0};
        ck_person original{.id = 7, .region = "eu", .name = "before", .revision = 0};

        auto& tracked = attached_set.attach(detached, std::move(original));
        require(attached_set.find(key) == &tracked, "attach registers the composite key in the identity map");

        attached_set.refresh(tracked);
        require(tracked.name == "winner" && tracked.revision == 1,
                "refresh reloads the composite row through its row_key");

        require(attached_ctx.save_changes().updated == 0, "a refreshed entity produces no update");
    }

    // The contexts above still hold the file, so it is removed once they are
    // destroyed.
    std::filesystem::remove(path);

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

    entityfy::db_context ctx{uri};
    ctx.execute("SET client_min_messages TO WARNING");
    // --- xmin system column as the concurrency token ---
    ctx.execute("DROP TABLE IF EXISTS \"rc_xmin_rows\" CASCADE");
    ctx.migrate<rc_xmin_row>();
    require(ctx.database().inspect_table("rc_xmin_rows").columns.size() == 2,
            "pg xmin model creates only the application columns");

    {
        entityfy::db_context writer{uri};
        auto writer_set = writer.set<rc_xmin_row>();
        auto& row = writer_set.add(rc_xmin_row{.body = "first"});
        require(writer.save_changes().inserted == 1, "pg xmin insert");
        require(row.revision != 0, "pg xmin token refreshed after the insert");

        // The write path must not assign the system column itself; the
        // refreshed token is what makes the next update match.
        row.body = "second";
        require(writer.save_changes().updated == 1, "pg xmin update after insert uses the refreshed token");

        const auto after_first_update = row.revision;
        row.body = "third";
        require(writer.save_changes().updated == 1, "pg xmin consecutive update keeps working");
        require(row.revision != after_first_update, "pg xmin token changes with every update");

        entityfy::db_context stale{uri};
        auto stale_set = stale.set<rc_xmin_row>();
        auto* stale_row = stale_set.find(row.id);
        require(stale_row != nullptr, "pg xmin stale context loads the row");

        row.body = "winner";
        require(writer.save_changes().updated == 1, "pg xmin winner update");

        stale_row->body = "loser";
        bool xmin_conflict = false;
        try
        {
            stale.save_changes();
        }
        catch(const entityfy::concurrency_error&)
        {
            xmin_conflict = true;
        }

        require(xmin_conflict, "pg xmin conflict after an out-of-band update");
    }

    ctx.execute("DROP TABLE IF EXISTS \"rc_xmin_rows\" CASCADE");

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

    const auto included = users.include<rc_post>({&ada, &grace});
    require(included.size() == 2 && included[0].second.size() == 2 && included[1].second.size() == 1,
            "pg include children");
    require(ada.posts.size() == 2 && grace.posts.size() == 1, "pg include fills navigation collections");
    require(ada.posts[0] == included[0].second[0], "pg navigation points at tracked child entities");

    const auto with_parents = posts.include_parent<rc_user>({&first_post, &second_post, &third_post});
    require(with_parents.size() == 3 && with_parents[0].second == &ada && with_parents[2].second == &grace,
            "pg include_parent mapping");

    const auto projected = users.query().order_by(&rc_user::name).select(&rc_user::id, &rc_user::name);
    require(projected.size() == 2 && std::get<0>(projected[0]) == ada.id &&
                std::get<1>(projected[0]) == "Ada Lovelace",
            "pg projection values");

    const auto raw = users.from_sql(
        "SELECT \"id\", \"email\", \"name\", \"version\" FROM \"rc_users\" WHERE \"email\" = 'ada@pg.test'");
    require(raw.size() == 1 && raw.front() == &ada, "pg raw sql materialization");

    entityfy::db_context concurrent{uri};
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
    catch(const entityfy::concurrency_error& error)
    {
        conflict = true;
        require(error.entries().size() == 1, "pg conflict entry count");
        require(error.entries().front().row_exists(), "pg conflict carries the database row");
        require(error.entries().front().token_column == "version", "pg conflict names the token column");
    }

    require(conflict, "pg optimistic concurrency conflict");

    // Client wins: take the database row as the original snapshot, keep the edit.
    const auto database_row = concurrent_users.database_values(*stale);
    require(database_row.has_value() && database_row->name == "Ada Winner", "pg database values are current");
    concurrent_users.accept_changes(*stale, *database_row);
    stale->name = "Ada Resolved";
    require(concurrent.save_changes().updated == 1, "pg client wins resolves the conflict");
    users.refresh(*fetched);
    require(fetched->name == "Ada Resolved", "pg refresh picks up the resolved value");

    // Stale delete: the token is part of the DELETE as well.
    {
        auto* tracked_grace = users.find(grace.id);
        require(tracked_grace != nullptr, "pg writer tracks grace");

        entityfy::db_context other{uri};
        auto other_users = other.set<rc_user>();
        auto* stale_grace = other_users.find(grace.id);
        require(stale_grace != nullptr, "pg stale context loads grace");

        tracked_grace->name = "Grace Winner";
        ctx.save_changes();

        other_users.remove(*stale_grace);
        bool delete_conflict = false;
        try
        {
            other.save_changes();
        }
        catch(const entityfy::concurrency_error&)
        {
            delete_conflict = true;
        }

        require(delete_conflict, "pg stale delete conflicts");
    }

    users.remove(*fetched);
    saved = ctx.save_changes();
    require(saved.removed == 1, "pg delete tracked");
    require(ctx.database().table<rc_user>().count() == 1, "pg row deleted");
    require(ctx.database().table<rc_post>().count() == 1, "pg delete cascade removed posts");

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
    require(ctx.database().table<rc_tag>().count() == 3, "pg batched rows exist");

    const auto pg_json_rows = ctx.query("SELECT \"payload\" FROM \"rc_tags\" WHERE \"id\" = 1");
    require(pg_json_rows.size() == 1 && std::get<std::string>(pg_json_rows.front().front()) == "{}",
            "pg json column round trip from database");

    ctx.transaction([&] {
        users.add(rc_user{.email = "tx@pg.test", .name = "Tx"});
        ctx.save_changes();
    });
    require(ctx.database().table<rc_user>().count() == 2, "pg transaction commit persists");

    auto children = ctx.set<rc_child>();
    auto& child = children.add(rc_child{.id = 10, .parent_id = 1, .label = "child"});

    auto parents = ctx.set<rc_parent>();
    auto& parent = parents.add(rc_parent{.id = 1, .name = "parent"});

    const auto dependency_saved = ctx.save_changes();
    require(dependency_saved.inserted == 2, "pg dependency order inserts both rows");
    require(ctx.database().table<rc_parent>().count() == 1, "pg parent row exists");
    require(ctx.database().table<rc_child>().count() == 1, "pg child row exists");

    children.remove(child);
    parents.remove(parent);

    const auto dependency_deleted = ctx.save_changes();
    require(dependency_deleted.removed == 2, "pg dependency order deletes both rows");
    require(ctx.database().table<rc_parent>().count() == 0, "pg parent row removed");
    require(ctx.database().table<rc_child>().count() == 0, "pg child row removed");

    // --- statement cache: named prepared statements on the server ---
    auto& database = ctx.database();
    database.set_statement_cache_capacity(32);
    database.clear_statement_cache();
    require(database.statement_cache_capacity() == 32, "pg cache capacity is configurable");

    (void)users.query().select(&rc_user::email);
    const auto cached_after_first = database.statement_cache_size();
    (void)users.query().select(&rc_user::email);
    require(cached_after_first == 1 && database.statement_cache_size() == 1,
            "pg cache prepares a statement once and reuses it");

    const auto cached_rows = users.query().order_by(&rc_user::id).select(&rc_user::id);
    database.set_statement_cache_capacity(0);
    require(database.statement_cache_size() == 0, "pg disabling the cache deallocates the plans");
    const auto uncached_rows = users.query().order_by(&rc_user::id).select(&rc_user::id);
    require(cached_rows == uncached_rows, "pg cache does not change results");

    database.set_statement_cache_capacity(32);
    (void)users.query().select(&rc_user::email);
    require(database.statement_cache_size() > 0, "pg cache is populated before DDL");
    ctx.execute("CREATE TEMPORARY TABLE \"rc_cache_probe\" (\"id\" INTEGER)");
    require(database.statement_cache_size() == 0, "pg DDL drops stale prepared plans");
    ctx.execute("DROP TABLE IF EXISTS \"rc_cache_probe\"");

    // PREPARE is transactional: a rolled back transaction removes the plan the
    // cache believes in, so the next call has to recover through the plain path.
    database.set_statement_cache_capacity(32);
    database.clear_statement_cache();

    bool cache_rollback = false;
    try
    {
        ctx.transaction([&] {
            (void)users.query().select(&rc_user::email);
            throw std::runtime_error{"force rollback"};
        });
    }
    catch(const std::exception&)
    {
        cache_rollback = true;
    }

    require(cache_rollback, "pg cache rollback probe ran");
    require(database.statement_cache_size() == 1, "pg cache holds the plan prepared inside the transaction");
    const auto after_rollback = users.query().select(&rc_user::email);
    require(!after_rollback.empty(), "pg query recovers after a rolled back PREPARE");
    require(database.statement_cache_size() == 1, "pg recovered plan is cached again");

    // --- composite key navigation ---
    ctx.execute("DROP TABLE IF EXISTS \"cn_children\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"cn_parents\" CASCADE");
    run_composite_navigation_scenario(ctx);
    ctx.execute("DROP TABLE IF EXISTS \"cn_children\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"cn_parents\" CASCADE");

    ctx.execute("DROP TABLE IF EXISTS \"rc_children\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_parents\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_tags\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_comments\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_posts\" CASCADE");
    ctx.execute("DROP TABLE IF EXISTS \"rc_users\" CASCADE");

    return status::passed;
}

status run_mysql_checks()
{
    const char* uri = std::getenv("ENTITYFY_MYSQL_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        std::cout << "mysql skipped: ENTITYFY_MYSQL_URI is not set\n";
        return status::skipped;
    }

    entityfy::db_context ctx{uri};

    for(const char* table : {"rc_children", "rc_parents", "rc_comments", "rc_posts", "rc_tags", "rc_users"})
    {
        ctx.execute(std::string{"DROP TABLE IF EXISTS \""} + table + "\"");
    }

    ctx.migrate<rc_user>();
    ctx.migrate<rc_post>();
    ctx.migrate<rc_comment>();
    ctx.migrate<rc_tag>();
    ctx.migrate<rc_parent>();
    ctx.migrate<rc_child>();

    auto users = ctx.set<rc_user>();
    auto& ada = users.add(rc_user{.email = "ada@mysql.test", .name = "Ada"});
    auto& grace = users.add(rc_user{.email = "grace@mysql.test", .name = "Grace"});

    require(ctx.save_changes().inserted == 2, "mysql insert count");
    require(users.query().sum(&rc_user::id) == std::optional<std::int64_t>{3}, "mysql aggregate sum");
    require(users.query().avg(&rc_user::id) == std::optional<double>{1.5}, "mysql aggregate avg");
    require(users.query().min(&rc_user::id) == std::optional<std::int64_t>{1}, "mysql aggregate min");
    require(users.query().max(&rc_user::id) == std::optional<std::int64_t>{2}, "mysql aggregate max");

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    const auto selected = users.query()
                              .where(entityfy::where(&rc_user::email).ends_with("@mysql.test"))
                              .order_by(&rc_user::name)
                              .take(1)
                              .to_list();
    require(selected.size() == 1 && selected.front() == &ada, "mysql filter/order/take");
    require(users.query().count() == 2, "mysql query count");

    auto posts = ctx.set<rc_post>();
    auto& first_post = posts.add(rc_post{.user_id = ada.id, .title = "first", .score = 10});
    posts.add(rc_post{.user_id = ada.id, .title = "second", .score = 20});
    posts.add(rc_post{.user_id = grace.id, .title = "third", .score = 5});
    require(ctx.save_changes().inserted == 3, "mysql posts inserted");

    const auto joined = users.join<rc_post>(&rc_user::id, &rc_post::user_id);
    require(joined.size() == 3, "mysql join row count");

    const auto counts = posts.query().count_by(&rc_post::user_id);
    require(counts.size() == 2 && counts[0].first == ada.id && counts[0].second == 2 &&
                counts[1].first == grace.id && counts[1].second == 1,
            "mysql count_by");

    const auto included = users.include<rc_post>({&ada, &grace});
    require(included.size() == 2 && included[0].second.size() == 2 && included[1].second.size() == 1, "mysql include");

    const auto projected = users.query().order_by(&rc_user::name).select(&rc_user::id, &rc_user::name);
    require(projected.size() == 2 && std::get<0>(projected[0]) == ada.id &&
                std::get<1>(projected[0]) == "Ada",
            "mysql projection values");

    const auto raw = users.from_sql(
        "SELECT \"id\", \"email\", \"name\", \"version\" FROM \"rc_users\" WHERE \"email\" = 'ada@mysql.test'");
    require(raw.size() == 1 && raw.front() == &ada, "mysql raw sql materialization");

    statements.clear();
    ada.name = "Ada Lovelace";
    require(ctx.save_changes().updated == 1, "mysql single update tracked");
    require(statements.size() == 1, "mysql single update statement");
    require(statements.front().find("\"name\" = ?") != std::string::npos, "mysql changed column is in update");
    require(statements.front().find("\"email\" = ?") == std::string::npos, "mysql unchanged column is not in update");

    users.remove(ada);
    require(ctx.save_changes().removed == 1, "mysql delete tracked");
    require(ctx.database().table<rc_post>().count() == 1, "mysql delete cascade removed posts");

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "one"});
    tags.add(rc_tag{.id = 2, .name = "two"});
    tags.add(rc_tag{.id = 3, .name = "three"});
    require(ctx.save_changes().inserted == 3, "mysql explicit-key entities inserted");
    require(ctx.database().table<rc_tag>().count() == 3, "mysql batched rows exist");

    const auto json_rows = ctx.query("SELECT \"payload\" FROM \"rc_tags\" WHERE \"id\" = 1");
    require(json_rows.size() == 1 && std::get<std::string>(json_rows.front().front()) == "{}",
            "mysql json column round trip");

    ctx.transaction([&] {
        users.add(rc_user{.email = "tx@mysql.test", .name = "Tx"});
        ctx.save_changes();
    });
    require(ctx.database().table<rc_user>().count() == 2, "mysql transaction commit persists");

    (void)first_post;

    for(const char* table : {"rc_children", "rc_parents", "rc_comments", "rc_posts", "rc_tags", "rc_users"})
    {
        ctx.execute(std::string{"DROP TABLE IF EXISTS \""} + table + "\"");
    }

    return status::passed;
}

status run_sqlserver_checks()
{
    const char* uri = std::getenv("ENTITYFY_SQLSERVER_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        std::cout << "sqlserver skipped: ENTITYFY_SQLSERVER_URI is not set\n";
        return status::skipped;
    }

    entityfy::db_context ctx{uri};

    for(const char* table : {"rc_children", "rc_parents", "rc_comments", "rc_posts", "rc_tags", "rc_users"})
    {
        ctx.execute(std::string{"IF OBJECT_ID(N'dbo."} + table + "', N'U') IS NOT NULL DROP TABLE dbo." + table);
    }

    ctx.migrate<rc_user>();
    ctx.migrate<rc_post>();
    ctx.migrate<rc_comment>();
    ctx.migrate<rc_tag>();
    ctx.migrate<rc_parent>();
    ctx.migrate<rc_child>();

    auto users = ctx.set<rc_user>();
    auto& ada = users.add(rc_user{.email = "ada@tsql.test", .name = "Ada"});
    auto& grace = users.add(rc_user{.email = "grace@tsql.test", .name = "Grace"});

    require(ctx.save_changes().inserted == 2, "sqlserver insert count");
    require(users.query().sum(&rc_user::id) == std::optional<std::int64_t>{3}, "sqlserver aggregate sum");
    require(users.query().avg(&rc_user::id) == std::optional<double>{1.5}, "sqlserver aggregate avg");

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    const auto selected = users.query()
                              .where(entityfy::where(&rc_user::email).ends_with("@tsql.test"))
                              .order_by(&rc_user::name)
                              .take(1)
                              .to_list();
    require(selected.size() == 1 && selected.front() == &ada, "sqlserver filter/order/take");

    auto posts = ctx.set<rc_post>();
    auto& first_post = posts.add(rc_post{.user_id = ada.id, .title = "first", .score = 10});
    posts.add(rc_post{.user_id = ada.id, .title = "second", .score = 20});
    posts.add(rc_post{.user_id = grace.id, .title = "third", .score = 5});
    require(ctx.save_changes().inserted == 3, "sqlserver posts inserted");

    const auto joined = users.join<rc_post>(&rc_user::id, &rc_post::user_id);
    require(joined.size() == 3, "sqlserver join row count");

    const auto counts = posts.query().count_by(&rc_post::user_id);
    require(counts.size() == 2 && counts[0].first == ada.id && counts[0].second == 2,
            "sqlserver count_by");

    const auto included = users.include<rc_post>({&ada, &grace});
    require(included.size() == 2 && included[0].second.size() == 2 && included[1].second.size() == 1,
            "sqlserver include");

    const auto projected = users.query().order_by(&rc_user::name).select(&rc_user::id, &rc_user::name);
    require(projected.size() == 2 && std::get<1>(projected[0]) == "Ada", "sqlserver projection values");

    const auto raw = users.from_sql(
        "SELECT \"id\", \"email\", \"name\", \"version\" FROM \"rc_users\" WHERE \"email\" = 'ada@tsql.test'");
    require(raw.size() == 1 && raw.front() == &ada, "sqlserver raw sql materialization");

    statements.clear();
    ada.name = "Ada Lovelace";
    require(ctx.save_changes().updated == 1, "sqlserver single update tracked");
    require(statements.size() == 1, "sqlserver single update statement");
    require(statements.front().find("\"name\" = ?") != std::string::npos, "sqlserver changed column is in update");

    users.remove(ada);
    require(ctx.save_changes().removed == 1, "sqlserver delete tracked");
    require(ctx.database().table<rc_post>().count() == 1, "sqlserver delete cascade removed posts");

    auto tags = ctx.set<rc_tag>();
    tags.add(rc_tag{.id = 1, .name = "one"});
    tags.add(rc_tag{.id = 2, .name = "two"});
    tags.add(rc_tag{.id = 3, .name = "three"});
    require(ctx.save_changes().inserted == 3, "sqlserver explicit-key entities inserted");
    require(ctx.database().table<rc_tag>().count() == 3, "sqlserver batched rows exist");

    const auto json_rows = ctx.query("SELECT \"payload\" FROM \"rc_tags\" WHERE \"id\" = 1");
    require(json_rows.size() == 1 && std::get<std::string>(json_rows.front().front()) == "{}",
            "sqlserver json column round trip");

    ctx.transaction([&] {
        users.add(rc_user{.email = "tx@tsql.test", .name = "Tx"});
        ctx.save_changes();
    });
    require(ctx.database().table<rc_user>().count() == 2, "sqlserver transaction commit persists");

    (void)first_post;

    for(const char* table : {"rc_children", "rc_parents", "rc_comments", "rc_posts", "rc_tags", "rc_users"})
    {
        ctx.execute(std::string{"IF OBJECT_ID(N'dbo."} + table + "', N'U') IS NOT NULL DROP TABLE dbo." + table);
    }

    return status::passed;
}

// Composite-key navigation over one context. The assertions are dialect
// neutral, so the SQLite run and the PostgreSQL run exercise the same
// behaviour: the tuple predicate and the loaders do not special-case a target.
void run_composite_navigation_scenario(entityfy::db_context& ctx)
{
    ctx.migrate<cn_parent>();
    ctx.migrate<cn_child>();

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    auto parents = ctx.set<cn_parent>();
    auto children = ctx.set<cn_child>();

    auto& one_a = parents.add(cn_parent{.tenant_id = 1, .code = "a", .name = "one-a"});
    auto& one_b = parents.add(cn_parent{.tenant_id = 1, .code = "b", .name = "one-b"});
    auto& two_a = parents.add(cn_parent{.tenant_id = 2, .code = "a", .name = "two-a"});
    require(ctx.save_changes().inserted == 3, "composite parents are inserted");

    auto& a1 = children.add(cn_child{.tenant_id = 1, .parent_code = "a", .label = "a1"});
    auto& a2 = children.add(cn_child{.tenant_id = 1, .parent_code = "a", .label = "a2"});
    auto& b1 = children.add(cn_child{.tenant_id = 1, .parent_code = "b", .label = "b1"});
    auto& c1 = children.add(cn_child{.tenant_id = 2, .parent_code = "a", .label = "two-a"});
    require(ctx.save_changes().inserted == 4, "composite children are inserted");
    require(a1.id != 0 && b1.id != 0 && c1.id != 0, "composite children got generated ids");

    const auto key_one_a = entityfy::row_key{entityfy::sql_value{std::int64_t{1}},
                                             entityfy::sql_value{std::string{"a"}}};

    require(parents.find(key_one_a) == &one_a, "find locates the composite parent");

    // include: the whole referenced tuple selects the children, so neither the
    // same tenant with another code nor the same code in another tenant leaks
    // into the bucket.
    statements.clear();
    const auto included = parents.include<cn_child>({&one_a});
    require(included.size() == 1 && included.front().first == &one_a, "composite include maps the parent");
    const auto& bucket = included.front().second;
    require(bucket.size() == 2, "composite include loads the key's children");
    require(std::ranges::find(bucket, &a1) != bucket.end() && std::ranges::find(bucket, &a2) != bucket.end(),
            "composite include loads exactly the key's children");
    require(std::ranges::find(bucket, &b1) == bucket.end(), "a same-tenant child with another code is excluded");
    require(std::ranges::find(bucket, &c1) == bucket.end(), "a same-code child from another tenant is excluded");
    require(statements.size() == 1 && statements.front().rfind("SELECT", 0) == 0,
            "composite include issues one select");
    require(statements.front().find("\"tenant_id\" = ? AND \"parent_code\" = ?") != std::string::npos,
            "a single parent key is a conjunction of every key column");
    require(one_a.children.is_loaded() && one_a.children.get() == bucket,
            "include caches the composite bucket on the lazy handle");

    // Two distinct parent keys become one row value IN, still bound in column
    // order.
    statements.clear();
    const auto multi = parents.include<cn_child>({&one_a, &two_a});
    require(multi.size() == 2, "composite include maps every parent");
    require(multi[0].second.size() == 2 && multi[1].second.size() == 1, "each composite owner gets its own bucket");
    require(std::ranges::find(multi[1].second, &c1) != multi[1].second.end(), "the second tenant gets its child");
    require(statements.size() == 1, "a multi-owner composite include issues one select");
    require(statements.front().find("(\"tenant_id\", \"parent_code\") IN ((?, ?), (?, ?))") != std::string::npos,
            "several parent keys are one row value IN bound in column order");

    // include_parent resolves the parent of a composite child by the whole
    // referenced tuple.
    statements.clear();
    const auto resolved = children.include_parent<cn_parent>({&a1, &b1, &c1});
    require(resolved.size() == 3, "composite include_parent maps every child");
    require(resolved[0].second == &one_a, "include_parent resolves the (1, a) parent");
    require(resolved[1].second == &one_b, "include_parent resolves the (1, b) parent");
    require(resolved[2].second == &two_a, "include_parent resolves the (2, a) parent");
    require(statements.size() == 1 && statements.front().rfind("SELECT", 0) == 0,
            "composite include_parent issues one select");
    require(statements.front().find("(\"tenant_id\", \"code\") IN ((?, ?), (?, ?), (?, ?))") != std::string::npos,
            "include_parent compares the whole referenced tuple");
    require(a1.parent.get() == &one_a, "include_parent caches the resolved parent on the handle");

    // Lazy collection: the handle is bound to every key part and filters on the
    // child's whole foreign key.
    require(one_a.children.keys().size() == 2, "the collection handle binds every owner key part");
    require(as_integer(one_a.children.keys()[0]) == 1 && std::get<std::string>(one_a.children.keys()[1]) == "a",
            "the collection handle binds the owner key in column order");

    one_a.children.reset();
    statements.clear();
    const auto& loaded = one_a.children.get();
    require(loaded.size() == 2, "the lazy collection loads the key's children");
    require(std::ranges::find(loaded, &b1) == loaded.end(), "the lazy collection does not load another key's child");
    require(statements.size() == 1, "the lazy collection issues one statement");
    require(statements.front().find("\"tenant_id\" = ? AND \"parent_code\" = ?") != std::string::npos,
            "the lazy collection filters on every foreign key column");

    statements.clear();
    require(&one_a.children.get() == &loaded, "the second collection access returns the cache");
    require(statements.empty(), "the second collection access issues no SQL");

    // Lazy reference: the handle carries every foreign key part and resolves
    // the same tracked parent find() returns.
    require(a1.parent.keys().size() == 2, "the reference handle binds every foreign key part");
    require(as_integer(a1.parent.keys()[0]) == 1 && std::get<std::string>(a1.parent.keys()[1]) == "a",
            "the reference handle binds the foreign key in column order");

    a1.parent.reset();
    statements.clear();
    auto* resolved_parent = a1.parent.get();
    require(resolved_parent == &one_a, "the lazy reference resolves the composite parent");
    require(resolved_parent == parents.find(key_one_a), "the lazy reference and find() agree on the parent");
    require(statements.empty(), "a tracked parent is served from the identity map");

    statements.clear();
    require(a1.parent.get() == resolved_parent, "the second reference access returns the cache");
    require(statements.empty(), "the second reference access issues no SQL");

    // A complete, existing tuple inserts; a child whose local tuple shares only
    // the tenant part is rejected by the composite foreign key.
    auto& valid = children.add(cn_child{.tenant_id = 1, .parent_code = "b", .label = "valid"});
    require(ctx.save_changes().inserted == 1, "a complete composite reference inserts");
    require(valid.id != 0, "the valid child got a generated id");

    children.add(cn_child{.tenant_id = 1, .parent_code = "zzz", .label = "orphan"});
    bool rejected = false;
    try
    {
        ctx.save_changes();
    }
    catch(const entityfy::foreign_key_violation&)
    {
        rejected = true;
    }
    require(rejected, "a partial-key orphan violates the composite foreign key");

    // The context may outlive the scenario (PostgreSQL drops the tables after
    // this function), so the logger must not keep a reference to a local.
    ctx.set_logger({});
}

status run_composite_navigation_checks()
{
    entityfy::db_context ctx{"sqlite://:memory:"};
    run_composite_navigation_scenario(ctx);

    return status::passed;
}

status run_lazy_loading_checks()
{
    constexpr const char* path = "rc_lazy.db";
    std::filesystem::remove(path);

    std::int64_t ada_id = 0;
    std::int64_t grace_id = 0;
    std::int64_t linus_id = 0;
    std::int64_t ada_post_id = 0;
    std::int64_t grace_post_id = 0;
    {
        // Seed rows and close the context, so the checks below bind the handles
        // through attach() (a materialized row) instead of add().
        entityfy::db_context seed{"sqlite://rc_lazy.db"};
        seed.migrate<lz_user>();
        seed.migrate<lz_post>();
        seed.migrate<lz_note>();

        auto users = seed.set<lz_user>();
        auto posts = seed.set<lz_post>();

        auto& ada = users.add(lz_user{.name = "Ada"});
        auto& grace = users.add(lz_user{.name = "Grace"});
        auto& linus = users.add(lz_user{.name = "Linus"});
        seed.save_changes();
        require(ada.id != 0, "seed owner got a generated id");
        ada_id = ada.id;
        grace_id = grace.id;
        linus_id = linus.id;

        auto& first = posts.add(lz_post{.user_id = ada.id, .title = "first"});
        posts.add(lz_post{.user_id = ada.id, .title = "second"});
        auto& grace_post = posts.add(lz_post{.user_id = grace.id, .title = "grace"});
        require(seed.save_changes().inserted == 3, "seed children inserted");
        ada_post_id = first.id;
        grace_post_id = grace_post.id;
    }

    entityfy::db_context ctx{"sqlite://rc_lazy.db"};

    std::vector<std::string> statements;
    ctx.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

    auto users = ctx.set<lz_user>();
    auto posts = ctx.set<lz_post>();
    auto notes = ctx.set<lz_note>();

    statements.clear();
    auto* ada = users.find(ada_id);
    require(ada != nullptr, "owner found");
    require(ada->posts.is_attached(), "find() binds the lazy collection");
    require_not(ada->posts.is_loaded(), "find() does not load the lazy collection");
    require(ada->posts.keys().size() == 1 && as_integer(ada->posts.keys().front()) == ada_id,
            "the handle is bound to the owner key");

    statements.clear();
    const auto& loaded = ada->posts.get();
    require(loaded.size() == 2, "first access loads every child");
    require(statements.size() == 1, "first access issues exactly one statement");
    require(statements.front().rfind("SELECT", 0) == 0, "the statement is a select");
    require(statements.front().find("\"user_id\" = ?") != std::string::npos,
            "the collection filters on the foreign key");
    require(ada->posts.is_loaded(), "the payload is cached");

    std::vector<std::string> titles;
    for(const auto* post: loaded)
    {
        titles.push_back(post->title);
        require(posts.find(post->id) == post, "loaded children are tracked by the context");
    }
    std::ranges::sort(titles);
    require(titles.size() == 2 && titles[0] == "first" && titles[1] == "second", "both children are loaded");

    statements.clear();
    const auto& again = ada->posts.get();
    require(&again == &loaded && again.size() == 2, "second access returns the cache");
    require(statements.empty(), "second access issues no SQL");

    require(users.state(*ada) == entityfy::entity_state::unchanged, "a lazy load leaves the owner unchanged");

    const auto after_load = ctx.save_changes();
    require(after_load.updated == 0 && after_load.inserted == 0 && after_load.removed == 0,
            "a lazy load does not dirty the tracker");

    // refresh() swaps in a fresh row, so the handle is bound again (with an
    // empty cache) and still loads through the owner key.
    users.refresh(*ada);
    require(ada->posts.is_attached(), "refresh() rebinds the handle");
    require_not(ada->posts.is_loaded(), "refresh() drops the cached payload");
    const auto& reloaded = ada->posts.get();
    require(reloaded.size() == 2, "the rebound handle loads the children again");

    // Reference handles resolve the row the foreign key points at. The parent
    // is attached through find(), so a reference and find() agree on identity.
    auto* ada_post = posts.find(ada_post_id);
    require(ada_post != nullptr, "the post is found");
    require(ada_post->author.is_attached(), "find() binds the lazy reference");
    require_not(ada_post->author.is_loaded(), "find() does not load the lazy reference");
    require(ada_post->author.keys().size() == 1 &&
                as_integer(ada_post->author.keys().front()) == ada_post->user_id,
            "the reference is bound to the foreign key");

    statements.clear();
    auto* ada_parent = ada_post->author.get();
    require(ada_parent == ada, "the reference resolves the tracked parent");
    require(ada_parent->name == "Ada", "the reference names the right parent");
    require(statements.empty(), "a tracked parent is served from the identity map");

    // Changing the foreign key must not keep the old parent cached: the save
    // resets and rebinds the handle to the value it wrote.
    ada_post->user_id = linus_id;
    require(ctx.save_changes().updated == 1, "the foreign key change is saved");
    require_not(ada_post->author.is_loaded(), "save() drops the stale reference cache");
    require(ada_post->author.keys().size() == 1 &&
                as_integer(ada_post->author.keys().front()) == linus_id,
            "save() rebinds the reference to the new foreign key");

    statements.clear();
    auto* linus = ada_post->author.get();
    require(linus != nullptr && linus->name == "Linus", "the rebound reference loads the new parent");
    require(statements.size() == 1, "the new parent is loaded with one statement");
    require(statements.front().rfind("SELECT", 0) == 0, "the reference load is a select");
    require(ada_post->author.is_loaded(), "the new parent is cached");
    require(users.find(ada_post->user_id) == linus, "both paths return the same tracked parent");

    statements.clear();
    require(ada_post->author.get() == linus, "the second reference access returns the cache");
    require(statements.empty(), "the second reference access issues no SQL");

    // First access to an untracked parent is exactly one SELECT, and the
    // second access never reaches the database.
    auto* grace_post = posts.find(grace_post_id);
    require(grace_post != nullptr, "the second post is found");
    require(grace_post->author.is_attached(), "attach() binds the lazy reference");
    require_not(grace_post->author.is_loaded(), "attach() does not load the lazy reference");

    statements.clear();
    auto* grace = grace_post->author.get();
    require(grace != nullptr && grace->name == "Grace", "the reference loads the parent");
    require(statements.size() == 1, "first reference access issues exactly one statement");
    require(statements.front().rfind("SELECT", 0) == 0, "the reference load is a select");
    require(statements.front().find("\"id\" = ?") != std::string::npos,
            "the reference looks the parent up by its key");
    require(grace_post->author.is_loaded(), "the loaded parent is cached");
    require(users.find(grace_post->user_id) == grace, "the reference and find() return the same pointer");

    statements.clear();
    require(grace_post->author.get() == grace, "the second reference access returns the cache");
    require(statements.empty(), "the second reference access issues no SQL");

    // A nullable foreign key has no parent: get() is nullptr, the handle counts
    // as loaded, dereferencing reports not_found, and no SQL is issued.
    auto& note = notes.add(lz_note{.user_id = std::nullopt, .title = "orphan"});
    require(ctx.save_changes().inserted == 1, "the null foreign key note is saved");
    require(note.id != 0, "the note got a generated id");

    statements.clear();
    require(note.author.get() == nullptr, "a null foreign key has no parent");
    require(note.author.is_loaded(), "a missing parent counts as loaded");
    require(statements.empty(), "a null foreign key issues no SQL");

    bool not_found = false;
    try
    {
        (void)*note.author;
    }
    catch(const entityfy::lazy_error& error)
    {
        not_found = error.reason() == entityfy::lazy_error_reason::not_found;
    }
    require(not_found, "dereferencing a missing parent reports lazy_error::not_found");
    require(statements.empty(), "operator* does not fall back to SQL");

    // A save that fails rolls the entity back; the handle must drop the parent
    // cached inside the failed transaction and follow the restored key.
    grace_post->user_id = 999999; // no such user: the UPDATE violates the foreign key
    bool constraint_failed = false;
    try
    {
        ctx.save_changes();
    }
    catch(const std::exception&)
    {
        constraint_failed = true;
    }

    require(constraint_failed, "a dangling foreign key fails the save");
    // save_changes() rollback restores the pre-save image, which still holds the
    // user's pending edit; the handle must follow that restored value instead
    // of the parent it cached before the failed transaction.
    require(grace_post->user_id == 999999, "rollback keeps the pending foreign key edit");
    require_not(grace_post->author.is_loaded(), "rollback_save drops the cached parent");
    require(grace_post->author.keys().size() == 1 &&
                as_integer(grace_post->author.keys().front()) == 999999,
            "rollback_save rebinds the handle to the restored foreign key");

    statements.clear();
    require(grace_post->author.get() == nullptr, "the rolled back reference does not return the cached parent");
    require(statements.size() == 1, "the rebound reference queries the restored key");
    require(grace_post->author.is_loaded(), "the missing parent is cached");
    require(grace_post->author.get() == nullptr, "the stale parent never comes back");
    require(statements.size() == 1, "the second access issues no SQL");

    // Restore the row and the handle for the rest of the scenario. refresh()
    // swaps in the stored row, which rebinds the reference to its foreign key.
    posts.refresh(*grace_post);
    require(grace_post->user_id == grace_id, "refresh() restores the stored foreign key");
    require(grace_post->author.keys().size() == 1 &&
                as_integer(grace_post->author.keys().front()) == grace_id,
            "refresh() rebinds the reference to the stored foreign key");
    require_not(grace_post->author.is_loaded(), "refresh() drops the cached payload");

    // No-tracking rows are plain values: their handles are never bound.
    const auto untracked = users.as_no_tracking().where(entityfy::where(&lz_user::id) == ada_id).to_list();
    require(untracked.size() == 1, "no-tracking read returns the owner");
    lz_user untracked_copy = untracked.front();
    require_not(untracked_copy.posts.is_attached(), "a no-tracking row has an unbound handle");

    bool not_attached = false;
    try
    {
        (void)untracked_copy.posts.get();
    }
    catch(const entityfy::lazy_error& error)
    {
        not_attached = error.reason() == entityfy::lazy_error_reason::not_attached;
    }
    require(not_attached, "no-tracking access reports lazy_error::not_attached");

    // The context owns the binding: a handle that outlives it must not follow
    // the dangling owner.
    lz_user orphan;
    {
        entityfy::db_context scoped{"sqlite://:memory:"};
        scoped.migrate<lz_user>();
        scoped.migrate<lz_post>();

        auto& added = scoped.set<lz_user>().add(lz_user{.name = "Scoped"});
        scoped.save_changes();
        orphan = added;
    }

    require_not(orphan.posts.is_attached(), "a destroyed context detaches the handle");

    bool destroyed = false;
    try
    {
        (void)orphan.posts.get();
    }
    catch(const entityfy::lazy_error& error)
    {
        destroyed = error.reason() == entityfy::lazy_error_reason::context_destroyed;
    }
    require(destroyed, "a destroyed context reports lazy_error::context_destroyed");

    // clear() keeps the context but drops the state the handle pointed at.
    entityfy::db_context cleared{"sqlite://:memory:"};
    cleared.migrate<lz_user>();
    cleared.migrate<lz_post>();

    auto& tracked = cleared.set<lz_user>().add(lz_user{.name = "Cleared"});
    cleared.save_changes();

    lz_user cleared_copy = tracked;
    cleared.clear();

    bool context_cleared = false;
    try
    {
        (void)cleared_copy.posts.get();
    }
    catch(const entityfy::lazy_error& error)
    {
        context_cleared = error.reason() == entityfy::lazy_error_reason::context_cleared;
    }
    require(context_cleared, "clear() reports lazy_error::context_cleared");

    // A pool lease release calls clear(); a handle bound on that lease must not
    // read through the pooled context afterwards.
    entityfy::db_context_pool pool{"sqlite://:memory:", 1};
    lz_user pool_orphan;
    {
        auto lease = pool.acquire();
        lease->migrate<lz_user>();
        lease->migrate<lz_post>();

        auto& pooled = lease->set<lz_user>().add(lz_user{.name = "Pooled"});
        lease->save_changes();
        pool_orphan = pooled;
        require(pool_orphan.posts.is_attached(), "a pool lease binds the handle");
    }

    bool lease_cleared = false;
    try
    {
        (void)pool_orphan.posts.get();
    }
    catch(const entityfy::lazy_error& error)
    {
        lease_cleared = error.reason() == entityfy::lazy_error_reason::context_cleared;
    }
    require(lease_cleared, "a released pool lease reports lazy_error::context_cleared");

    // A generated key is assigned during save, after the handle was bound, so
    // the handle has to follow the id the database assigned.
    auto& fresh = users.add(lz_user{.name = "Fresh"});
    ctx.save_changes();
    require(fresh.id != 0, "generated id assigned");
    require(fresh.posts.is_attached(), "save() binds the added entity");
    require(fresh.posts.keys().size() == 1 && as_integer(fresh.posts.keys().front()) == fresh.id,
            "the handle follows the generated key");

    statements.clear();
    const auto& no_children = fresh.posts.get();
    require(no_children.empty(), "an owner without children loads an empty collection");
    require(statements.size() == 1, "the empty collection is one statement, not an error");

    // A relation that names only one column of a two-column key does not point
    // at the primary key, so binding it is an explicit error.
    auto pairs = ctx.set<lz_pair_row>();
    bool composite = false;
    try
    {
        pairs.add(lz_pair_row{.id = 1, .region = 2});
    }
    catch(const std::runtime_error& error)
    {
        composite = std::string{error.what()}.find("does not point at the primary key") != std::string::npos;
    }
    require(composite, "a relation to part of a composite key is an explicit error");

    // The reference direction of the same shape.
    auto pair_children = ctx.set<lz_pair_child>();
    bool composite_parent = false;
    try
    {
        pair_children.add(lz_pair_child{.pair_id = 1});
    }
    catch(const std::runtime_error& error)
    {
        composite_parent = std::string{error.what()}.find("does not point at the primary key") != std::string::npos;
    }
    require(composite_parent, "a reference to part of a composite key is an explicit error");

    // Eager loading fills the lazy handle from the rows the include query
    // already returned, so a later access never issues a second query. The
    // handle is seeded with an empty payload the include has to replace, so a
    // pass cannot come from a payload that was already going to be returned.
    require(ada->posts.is_attached(), "the owner's handle is bound before the include");
    ada->posts.mark_loaded(std::vector<lz_post*>{});
    require(ada->posts.is_loaded() && ada->posts.empty(), "the handle starts with a stale empty payload");

    statements.clear();
    const auto included = users.include<lz_post>({ada});
    require(included.size() == 1 && included.front().first == ada, "include maps the owner");
    require(included.front().second.size() == 1, "include returns the owner's children");
    require(ada->posts.is_loaded(), "include fills the lazy collection cache");
    require(ada->posts.is_attached(), "include keeps the lazy collection bound");
    require(ada->posts.size() == 1 && ada->posts.get() == included.front().second,
            "the cached collection is exactly the included bucket");
    require(statements.size() == 1 && statements.front().rfind("SELECT", 0) == 0,
            "include issues exactly one select");
    require(posts.find(ada->posts.get().front()->id) == ada->posts.get().front(),
            "included children are the tracked rows");
    require(statements.size() == 1, "the identity check adds no SQL");

    statements.clear();
    require(ada->posts.size() == 1 && ada->posts.get() == included.front().second,
            "a cached include collection is returned again");
    require(statements.empty(), "the included collection issues no second select");

    // Every owner caches its own bucket, including an owner without children:
    // an empty collection is a loaded payload, not a pending query.
    statements.clear();
    const auto multi = users.include<lz_post>({ada, grace, &fresh});
    require(multi.size() == 3, "include maps every owner");
    require(ada->posts.size() == 1 && grace->posts.size() == 1, "each owner caches its own bucket");
    require(ada->posts.get().front()->title == "second" && grace->posts.get().front()->title == "grace",
            "the cached buckets hold the right children");
    require(fresh.posts.is_loaded() && fresh.posts.empty(), "an owner without children caches an empty bucket");
    require(statements.size() == 1, "a multi-owner include issues one select");

    statements.clear();
    require_not(grace_post->author.is_loaded(), "the reference starts unloaded");
    const auto included_parents = posts.include_parent<lz_user>({grace_post});
    require(included_parents.size() == 1 && included_parents.front().first == grace_post,
            "include_parent maps the child");
    require(grace_post->author.is_loaded(), "include_parent fills the lazy reference cache");
    require(grace_post->author.is_attached(), "include_parent keeps the lazy reference bound");
    require(grace_post->author.get() == included_parents.front().second,
            "the cached reference is the included parent");
    require(grace_post->author.get() == users.find(grace_post->user_id),
            "the included parent is the tracked parent find() returns");
    require(statements.size() == 1 && statements.front().rfind("SELECT", 0) == 0,
            "include_parent issues exactly one select");

    statements.clear();
    require(grace_post->author.get() == included_parents.front().second,
            "a cached include reference is returned again");
    require(statements.empty(), "the included reference issues no second select");

    // mark_loaded() only fills the cache, so the binding is still the one the
    // tracker gave the handle: reset() makes it load through its own query.
    ada->posts.reset();
    require_not(ada->posts.is_loaded(), "reset() drops the included payload");
    require(ada->posts.is_attached(), "the included handle is still bound");

    statements.clear();
    const auto& bound_reload = ada->posts.get();
    require(bound_reload.size() == 1, "the preserved binding still loads the owner's children");
    require(statements.size() == 1, "the rebound handle loads with one query");

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
        {"ef_surface", run_ef_surface_checks},
        {"sqlite", run_sqlite_checks},
        {"composite_key", run_composite_key_checks},
        {"rollback", run_rollback_checks},
        {"concurrency", run_concurrency_checks},
        {"concurrency_resolution", run_concurrency_resolution_checks},
        {"attach_delete_conflict", run_attach_and_delete_conflict_checks},
        {"token_annotation", run_token_annotation_checks},
        {"multi_conflict", run_multi_conflict_checks},
        {"tracking_edges", run_tracking_edge_checks},
        {"transaction", run_transaction_checks},
        {"state", run_state_checks},
        {"retry", run_retry_checks},
        {"column_alias", run_column_alias_checks},
        {"column_alias_version", run_column_alias_version_checks},
        {"no_tracking", run_no_tracking_checks},
        {"lazy_loading", run_lazy_loading_checks},
        {"composite_navigation", run_composite_navigation_checks},
        {"manual_tracking", run_manual_tracking_checks},
        {"batch", run_batch_checks},
        {"dependency", run_dependency_checks},
        {"pool", run_pool_checks},
        {"async", run_async_checks},
        {"async_drain", run_async_drain_checks},
        {"postgres", run_postgres_checks},
        {"mysql", run_mysql_checks},
        {"sqlserver", run_sqlserver_checks},
    };

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    const char* only_scenario = std::getenv("ENTITYFY_ONLY_SCENARIO");

    for(const auto& current: entries)
    {
        if(only_scenario != nullptr && std::string_view{only_scenario} != current.name)
        {
            continue;
        }

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
