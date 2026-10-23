#include <catch2/catch_test_macros.hpp>

#include <entityfy/detail/backend.hpp>
#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("postgres placeholders rewrite generated question-mark binds", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT * FROM \"users\" WHERE \"email\" = ? AND \"id\" = ?"
        ) == "SELECT * FROM \"users\" WHERE \"email\" = $1 AND \"id\" = $2"
    );
}

TEST_CASE("postgres placeholders preserve native dollar binds", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT * FROM \"posts\" WHERE \"metadata\" ? 'tags' AND \"id\" = $1"
        ) == "SELECT * FROM \"posts\" WHERE \"metadata\" ? 'tags' AND \"id\" = $1"
    );
}

TEST_CASE("postgres placeholders ignore dollar binds inside quoted SQL", "[postgresql][statement]")
{
    REQUIRE(
        entityfy::detail::postgres_placeholders(
            "SELECT '$1', $$?$$, \"metadata\" ? 'tags' WHERE \"id\" = ?"
        ) == "SELECT '$1', $$?$$, \"metadata\" $1 'tags' WHERE \"id\" = $2"
    );
}

namespace
{

struct [[= entityfy::table{"pg_batch_items"}]] pg_batch_item
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{60}]]
    std::string name;
};

// Composite key: both parts are declared primary keys and there is no auto
// increment, so every insert carries the full identity.
struct [[= entityfy::table{"pg_ck_people"}]] pg_ck_person
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{16}]]
    std::string region;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string title;

    [[= entityfy::not_null]]
    std::int64_t score = 0;
};

// One non-integral key part.
struct [[= entityfy::table{"pg_ck_slugs"}]] pg_ck_slug
{
    [[= entityfy::primary_key, = entityfy::varchar{64}]]
    std::string slug;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

[[nodiscard]] entityfy::row_key pg_person_key(std::int64_t id, std::string region)
{
    return entityfy::row_key{entityfy::sql_value{id}, entityfy::sql_value{std::move(region)}};
}

[[nodiscard]] entityfy::row_key pg_slug_key(std::string slug)
{
    return entityfy::row_key{entityfy::sql_value{std::move(slug)}};
}

} // namespace

TEST_CASE("postgres insert_many pipelines multiple chunks", "[postgresql][backend][batch]")
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_batch_items\""});
    db.migrate<pg_batch_item>();

    // Ten rows per chunk, so the insert needs forty statements in one pipeline.
    db.set_batch_parameter_limit(20);

    std::vector<pg_batch_item> models;
    models.reserve(400);

    for(std::int64_t index = 0; index < 400; ++index)
    {
        models.push_back(pg_batch_item{.id = index + 1, .name = "item " + std::to_string(index)});
    }

    const auto inserted = db.insert_many(models);
    REQUIRE(inserted.rows_affected == 400);
    REQUIRE(db.count<pg_batch_item>() == 400);

    const auto loaded = db.find<pg_batch_item>(std::int64_t{400});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "item 399");

    // A batch inside an explicit transaction keeps the nested pipeline valid.
    std::vector<pg_batch_item> more;
    more.reserve(400);
    for(std::int64_t index = 0; index < 400; ++index)
    {
        more.push_back(pg_batch_item{.id = index + 1001, .name = "more " + std::to_string(index)});
    }

    db.transaction([&] { db.insert_many(more); });
    REQUIRE(db.count<pg_batch_item>() == 800);

    // A failure in a later chunk still surfaces as the classified error.
    models.back().id = 1;
    REQUIRE_THROWS_AS(db.insert_many(models), entityfy::unique_violation);

    // The connection must still be usable after the failed pipeline.
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_batch_items\""});
    REQUIRE_FALSE(db.inspect_table("pg_batch_items").exists());
}

TEST_CASE("postgres serialization failure carries the typed error", "[postgresql][backend][transient]")
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client setup{uri};
    setup.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_serial_race\""});
    setup.execute(entityfy::statement{.sql =
                                          "CREATE TABLE \"pg_serial_race\" (\"id\" INTEGER PRIMARY KEY, "
                                          "\"value\" INTEGER NOT NULL)"});
    setup.execute(entityfy::statement{.sql = "INSERT INTO \"pg_serial_race\" VALUES (1, 0), (2, 0)"});

    entityfy::client first{uri};
    entityfy::client second{uri};

    first.execute(entityfy::statement{.sql = "BEGIN ISOLATION LEVEL SERIALIZABLE"});
    second.execute(entityfy::statement{.sql = "BEGIN ISOLATION LEVEL SERIALIZABLE"});

    (void)first.query(entityfy::statement{.sql = "SELECT SUM(\"value\") FROM \"pg_serial_race\""});
    (void)second.query(entityfy::statement{.sql = "SELECT SUM(\"value\") FROM \"pg_serial_race\""});

    first.execute(entityfy::statement{.sql = "UPDATE \"pg_serial_race\" SET \"value\" = 1 WHERE \"id\" = 1"});
    second.execute(entityfy::statement{.sql = "UPDATE \"pg_serial_race\" SET \"value\" = 1 WHERE \"id\" = 2"});

    first.execute(entityfy::statement{.sql = "COMMIT"});

    REQUIRE_THROWS_AS(second.execute(entityfy::statement{.sql = "COMMIT"}), entityfy::serialization_failure);

    second.execute(entityfy::statement{.sql = "ROLLBACK"});
    setup.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_serial_race\""});
}

TEST_CASE("postgres insert_many keeps a one entry statement cache valid",
          "[postgresql][backend][batch][cache]")
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_batch_items\""});
    db.migrate<pg_batch_item>();

    // Ten rows per chunk; 401 rows make the last chunk shorter, so the batch has
    // two distinct SQL texts while the cache only holds one plan.
    db.set_batch_parameter_limit(20);
    db.set_statement_cache_capacity(1);

    std::vector<pg_batch_item> models;
    models.reserve(401);

    for(std::int64_t index = 0; index < 401; ++index)
    {
        models.push_back(pg_batch_item{.id = index + 1, .name = "item " + std::to_string(index)});
    }

    const auto inserted = db.insert_many(models);
    REQUIRE(inserted.rows_affected == 401);
    REQUIRE(db.count<pg_batch_item>() == 401);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_batch_items\""});
}

TEST_CASE("postgres composite and string keys", "[postgresql][backend][composite]")
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};

    // The tables belong to this case only, so they are dropped before and after
    // it and the database stays clean either way.
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_ck_people\""});
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_ck_slugs\""});

    db.migrate<pg_ck_person>();
    db.migrate<pg_ck_slug>();

    REQUIRE(db.inspect_table("pg_ck_people").exists());
    REQUIRE(db.inspect_table("pg_ck_slugs").exists());

    const auto key_columns = entityfy::primary_key_columns<pg_ck_person>(db.target_dialect());
    REQUIRE(key_columns.size() == 2);
    REQUIRE(key_columns[0].name == "id");
    REQUIRE(key_columns[1].name == "region");

    // Composite key CRUD.
    REQUIRE(db.insert(pg_ck_person{.id = 1, .region = "eu", .title = "alpha", .score = 10}).rows_affected == 1);

    const auto inserted = db.insert_many(std::vector<pg_ck_person>{
        {.id = 2, .region = "eu", .title = "beta", .score = 20},
        {.id = 1, .region = "us", .title = "gamma", .score = 30},
    });
    REQUIRE(inserted.rows_affected == 2);
    REQUIRE(db.count<pg_ck_person>() == 3);

    // The pair is the uniqueness constraint, not either column alone.
    REQUIRE_THROWS_AS(
        db.insert(pg_ck_person{.id = 1, .region = "us", .title = "duplicate", .score = 0}),
        entityfy::unique_violation
    );

    const auto loaded = db.find<pg_ck_person>(pg_person_key(1, "us"));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->title == "gamma");
    REQUIRE(loaded->score == 30);

    REQUIRE_FALSE(db.find<pg_ck_person>(pg_person_key(1, "apac")).has_value());
    REQUIRE_FALSE(db.find<pg_ck_person>(pg_person_key(9, "us")).has_value());

    auto updated = *loaded;
    updated.title = "gamma-renamed";
    updated.score = 31;
    REQUIRE(db.update(updated) == 1);

    const auto reread = db.find<pg_ck_person>(pg_person_key(1, "us"));
    REQUIRE(reread.has_value());
    REQUIRE(reread->title == "gamma-renamed");
    REQUIRE(reread->score == 31);

    // The row that shares the id part keeps its own values.
    const auto sibling = db.find<pg_ck_person>(pg_person_key(1, "eu"));
    REQUIRE(sibling.has_value());
    REQUIRE(sibling->title == "alpha");

    REQUIRE(db.remove_by_key<pg_ck_person>(pg_person_key(1, "us")) == 1);
    REQUIRE(db.count<pg_ck_person>() == 2);
    REQUIRE_FALSE(db.find<pg_ck_person>(pg_person_key(1, "us")).has_value());
    REQUIRE(db.find<pg_ck_person>(pg_person_key(1, "eu")).has_value());
    REQUIRE(db.remove_by_key<pg_ck_person>(pg_person_key(1, "us")) == 0);

    // A filter on a non-key column still selects the composite rows.
    const auto eu_rows = db.find_many<pg_ck_person>(entityfy::where(&pg_ck_person::region).eq(std::string{"eu"}));
    REQUIRE(eu_rows.size() == 2);

    // String key CRUD.
    REQUIRE(db.insert(pg_ck_slug{.slug = "entityfy", .name = "Entityfy"}).rows_affected == 1);
    REQUIRE_THROWS_AS(db.insert(pg_ck_slug{.slug = "entityfy", .name = "Duplicate"}), entityfy::unique_violation);

    const auto by_slug = db.find<pg_ck_slug>(pg_slug_key("entityfy"));
    REQUIRE(by_slug.has_value());
    REQUIRE(by_slug->name == "Entityfy");

    REQUIRE_FALSE(db.find<pg_ck_slug>(pg_slug_key("missing")).has_value());

    auto renamed = *by_slug;
    renamed.name = "Entityfy ORM";
    REQUIRE(db.update(renamed) == 1);

    const auto slug_reread = db.find<pg_ck_slug>(pg_slug_key("entityfy"));
    REQUIRE(slug_reread.has_value());
    REQUIRE(slug_reread->name == "Entityfy ORM");

    REQUIRE(db.remove_by_key<pg_ck_slug>(pg_slug_key("entityfy")) == 1);
    REQUIRE(db.count<pg_ck_slug>() == 0);
    REQUIRE(db.remove_by_key<pg_ck_slug>(pg_slug_key("entityfy")) == 0);

    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_ck_people\""});
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"pg_ck_slugs\""});

    REQUIRE_FALSE(db.inspect_table("pg_ck_people").exists());
    REQUIRE_FALSE(db.inspect_table("pg_ck_slugs").exists());
}
