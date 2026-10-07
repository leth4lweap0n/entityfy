#include "support/Fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

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
