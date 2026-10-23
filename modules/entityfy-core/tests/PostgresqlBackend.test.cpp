#include <catch2/catch_test_macros.hpp>

#include <entityfy/detail/backend.hpp>
#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
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
