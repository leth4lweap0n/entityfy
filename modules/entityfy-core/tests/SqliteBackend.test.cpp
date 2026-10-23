#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <sqlite3.h>

TEST_CASE("sqlite backend enforces declared foreign keys", "[sqlite][constraints]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<entityfy::test::User>();
    db.migrate<entityfy::test::Post>();

    REQUIRE_THROWS_AS(
        db.insert<entityfy::test::Post>({
            .user_id = 404,
            .title = "orphaned post",
        }),
        entityfy::foreign_key_violation
    );
}

TEST_CASE("sqlite migrations roll back failed transactional batches", "[sqlite][migration]")
{
    entityfy::client db{"sqlite://:memory:"};

    REQUIRE_THROWS_AS(
        db.apply_migrations({
            entityfy::migration{
                .id = "001_failed",
                .statements = {
                    entityfy::statement{.sql = "CREATE TABLE \"rolled_back\" (\"id\" INTEGER PRIMARY KEY)"},
                    entityfy::statement{.sql = "THIS IS NOT SQL"},
                },
            },
        }),
        entityfy::migration_error
    );

    REQUIRE_FALSE(db.inspect_table("rolled_back").exists());
}

TEST_CASE("sqlite busy maps to the retryable error", "[sqlite][backend][transient]")
{
    const auto path = (std::filesystem::temp_directory_path() / "entityfy_sqlite_busy.db").string();
    std::filesystem::remove(path);

    {
        entityfy::client db{"sqlite://" + path};
        db.execute(entityfy::statement{.sql = "CREATE TABLE \"busy_items\" (\"id\" INTEGER PRIMARY KEY)"});
        db.execute(entityfy::statement{.sql = "PRAGMA busy_timeout = 10"});

        sqlite3* other = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &other) == SQLITE_OK);
        REQUIRE(sqlite3_exec(other, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK);

        REQUIRE_THROWS_AS(
            db.execute(entityfy::statement{.sql = "INSERT INTO \"busy_items\" (\"id\") VALUES (1)"}),
            entityfy::database_busy);

        sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(other);
    }

    std::filesystem::remove(path);
}
