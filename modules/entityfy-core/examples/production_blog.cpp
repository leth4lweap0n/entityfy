#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

enum class PostStatus : std::int32_t
{
    draft = 0,
    published = 1,
    archived = 2,
};

struct [[= entityfy::table{"accounts"}]] Account
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::not_null, = entityfy::varchar{40}, = entityfy::default_value{"'free'"}]]
    std::string plan = "free";

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"users"}]] User
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"accounts", "id", "CASCADE", "CASCADE"}]]
    std::int64_t account_id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string display_name;

    [[= entityfy::nullable, = entityfy::text]]
    std::optional<std::string> bio;

    [[= entityfy::json, = entityfy::not_null, = entityfy::default_value{"'{}'"}]]
    std::string settings_json = "{}";

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"posts"}]] Post
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"users", "id", "CASCADE", "CASCADE"}]]
    std::int64_t author_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{200}]]
    std::string title;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{220}]]
    std::string slug;

    [[= entityfy::nullable, = entityfy::varchar{280}]]
    std::optional<std::string> summary;

    [[= entityfy::text, = entityfy::not_null]]
    std::string body;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::default_value{"0"}]]
    PostStatus status = PostStatus::draft;

    [[= entityfy::nullable, = entityfy::date]]
    std::optional<std::chrono::sys_days> published_on;

    [[= entityfy::json, = entityfy::not_null, = entityfy::default_value{"'{}'"}]]
    std::string metadata_json = "{}";

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"comments"}]] Comment
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"posts", "id", "CASCADE", "CASCADE"}]]
    std::int64_t post_id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"users", "id", "CASCADE", "CASCADE"}]]
    std::int64_t author_id = 0;

    [[= entityfy::text, = entityfy::not_null]]
    std::string body;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::default_value{"0"}]]
    bool approved = false;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};
};

int main()
{
    entityfy::client db{"sqlite://:memory:"};

    db.transaction([](entityfy::client& tx) {
        tx.migrate<Account>();
        tx.migrate<User>();
        tx.migrate<Post>();
        tx.migrate<Comment>();
    });

    db.apply_migrations({
        entityfy::migration{
            .id = "001_published_post_summaries",
            .statements = {
                entityfy::statement{
                    .sql =
                        "CREATE VIEW IF NOT EXISTS \"published_post_summaries\" AS "
                        "SELECT \"posts\".\"id\", \"posts\".\"title\", \"users\".\"email\" AS \"author_email\" "
                        "FROM \"posts\" JOIN \"users\" ON \"users\".\"id\" = \"posts\".\"author_id\" "
                        "WHERE \"posts\".\"status\" = 1",
                },
            },
        },
    });

    auto accounts = db.table<Account>();
    auto users = db.table<User>();
    auto posts = db.table<Post>();
    auto comments = db.table<Comment>();

    const auto account = accounts.insert(Account{.name = "Entityfy Labs", .plan = "team"});
    const auto ada = users.insert(User{
        .account_id = account.last_insert_id,
        .email = "ada@entityfy.test",
        .display_name = "Ada",
        .bio = "Computing notes and database design.",
        .settings_json = R"({"theme":"light","digest":true})",
    });

    const auto post = posts.insert(Post{
        .author_id = ada.last_insert_id,
        .title = "Designing a reflection-first ORM",
        .slug = "reflection-first-orm",
        .summary = "A production schema with typed filters, relations, JSON, dates, and migrations.",
        .body = "Entityfy keeps C++ models as the source of truth while still emitting explicit SQL.",
        .status = PostStatus::published,
        .published_on = std::chrono::sys_days{std::chrono::year{2026} / 5 / 20},
        .metadata_json = R"({"tags":["orm","cpp","reflection"],"readingMinutes":6})",
    });

    [[maybe_unused]] const auto inserted_comments = comments.insert_many({
        Comment{.post_id = post.last_insert_id, .author_id = ada.last_insert_id, .body = "First approved comment.", .approved = true},
        Comment{.post_id = post.last_insert_id, .author_id = ada.last_insert_id, .body = "Needs moderation.", .approved = false},
    });

    const auto published_posts =
        posts.find_many(
            entityfy::query<Post>(
                entityfy::where(&Post::status).eq(PostStatus::published) &&
                entityfy::where(&Post::summary).is_not_null()
            )
                .order_by_desc(&Post::created_at)
                .take(10)
        );

    for(const auto& published_post: published_posts)
    {
        const auto author = posts.belongs_to<User>(published_post, &Post::author_id);
        const auto approved_comments = comments.count(
            entityfy::where(&Comment::post_id).eq(published_post.id) &&
            entityfy::where(&Comment::approved).eq(true)
        );

        spdlog::info(
            "{} by {} ({} approved comments)",
            published_post.title,
            author ? author->email : "unknown",
            approved_comments
        );
    }

    if(auto existing = users.find(ada.last_insert_id))
    {
        existing->display_name = "Ada Lovelace";
        [[maybe_unused]] const auto updated_users = users.update(*existing);
    }

    if(auto saved_account = accounts.find(account.last_insert_id))
    {
        const auto account_users = accounts.has_many<User>(*saved_account, &User::account_id);
        spdlog::info("account has {} user(s)", account_users.size());
    }
}
