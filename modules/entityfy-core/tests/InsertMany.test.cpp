#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace
{

struct [[= entityfy::table{"batch_items"}]] batch_item
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

struct [[= entityfy::table{"generated_items"}]] generated_item
{
    [[= entityfy::primary_key, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

void collect_statements(entityfy::client& db, std::vector<std::string>& statements)
{
    db.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });
}

std::size_t count_inserts(const std::vector<std::string>& statements)
{
    std::size_t total = 0;
    for(const auto& statement: statements)
    {
        if(statement.find("INSERT INTO") != std::string::npos)
        {
            ++total;
        }
    }

    return total;
}

} // namespace

TEST_CASE("insert_many writes explicit keys with a single statement", "[core][insert]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<batch_item>();

    std::vector<std::string> statements;
    collect_statements(db, statements);

    const std::vector<batch_item> items{
        {.id = 1, .name = "one"},
        {.id = 2, .name = "two"},
        {.id = 3, .name = "three"},
    };

    const auto result = db.insert_many<batch_item>(items);
    REQUIRE(result.rows_affected == 3);
    REQUIRE(count_inserts(statements) == 1);
    REQUIRE(statements.front().find("), (") != std::string::npos);
    REQUIRE(db.count<batch_item>() == 3);
}

TEST_CASE("insert_many splits batches by the parameter limit", "[core][insert]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<batch_item>();
    db.set_batch_parameter_limit(4); // 2 columns -> 2 rows per chunk

    std::vector<std::string> statements;
    collect_statements(db, statements);

    const std::vector<batch_item> items{
        {.id = 1, .name = "one"},
        {.id = 2, .name = "two"},
        {.id = 3, .name = "three"},
        {.id = 4, .name = "four"},
    };

    const auto result = db.insert_many<batch_item>(items);
    REQUIRE(result.rows_affected == 4);
    REQUIRE(count_inserts(statements) == 2);
    REQUIRE(db.count<batch_item>() == 4);
}

TEST_CASE("insert_many keeps generated keys row by row", "[core][insert]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<generated_item>();

    std::vector<std::string> statements;
    collect_statements(db, statements);

    const std::vector<generated_item> items{
        {.name = "one"},
        {.name = "two"},
        {.name = "three"},
    };

    const auto result = db.insert_many<generated_item>(items);
    REQUIRE(result.rows_affected == 3);
    REQUIRE(count_inserts(statements) == 3);
    REQUIRE(db.count<generated_item>() == 3);
}

TEST_CASE("the SQL logger receives every executed statement", "[core][logger]")
{
    std::vector<std::string> statements;

    {
        entityfy::client db{"sqlite://:memory:"};
        db.set_logger([&](std::string_view sql) { statements.emplace_back(sql); });

        db.migrate<batch_item>();
        db.insert<batch_item>({.id = 1, .name = "one"});
        db.query(entityfy::statement{.sql = "SELECT \"id\" FROM \"batch_items\""});

        REQUIRE(statements.size() >= 4);

        bool created = false;
        for(const auto& statement: statements)
        {
            created = created || statement.find("CREATE TABLE") != std::string::npos;
        }

        REQUIRE(created);
        REQUIRE(statements.back().find("SELECT") != std::string::npos);
    }
}
