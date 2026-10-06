#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace
{

entityfy::statement insert_item(std::int64_t id, std::string name)
{
    return entityfy::statement{
        .sql = "INSERT INTO \"cache_items\" (\"id\", \"name\") VALUES (?, ?)",
        .binds = {id, std::move(name)},
    };
}

entityfy::statement select_items()
{
    return entityfy::statement{.sql = "SELECT \"id\", \"name\" FROM \"cache_items\" ORDER BY \"id\" ASC"};
}

entityfy::client make_client()
{
    entityfy::client db{"sqlite://:memory:"};
    db.execute(entityfy::statement{.sql =
                                       "CREATE TABLE \"cache_items\" (\"id\" INTEGER PRIMARY KEY, "
                                       "\"name\" TEXT NOT NULL)"});
    db.clear_statement_cache();
    return db;
}

} // namespace

TEST_CASE("statement cache compiles every distinct statement once", "[sqlite][cache]")
{
    auto db = make_client();

    REQUIRE(db.statement_cache_size() == 0);
    REQUIRE(db.statement_cache_capacity() > 0);

    db.execute(insert_item(1, "first"));
    REQUIRE(db.statement_cache_size() == 1);

    db.execute(insert_item(2, "second"));
    REQUIRE(db.statement_cache_size() == 1);

    db.query(select_items());
    REQUIRE(db.statement_cache_size() == 2);
}

TEST_CASE("statement cache never leaks bind values between executions", "[sqlite][cache]")
{
    auto db = make_client();

    db.execute(insert_item(1, "first"));
    db.execute(insert_item(2, "second"));
    db.execute(insert_item(3, "third"));

    const auto rows = db.query(select_items());
    REQUIRE(rows.size() == 3);
    REQUIRE(std::get<std::int64_t>(rows[0][0]) == 1);
    REQUIRE(std::get<std::string>(rows[0][1]) == "first");
    REQUIRE(std::get<std::int64_t>(rows[1][0]) == 2);
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
    REQUIRE(std::get<std::int64_t>(rows[2][0]) == 3);
    REQUIRE(std::get<std::string>(rows[2][1]) == "third");
}

TEST_CASE("statement cache evicts the least recently used statement", "[sqlite][cache]")
{
    auto db = make_client();
    db.set_statement_cache_capacity(1);

    db.execute(insert_item(1, "first"));
    REQUIRE(db.statement_cache_size() == 1);

    db.query(select_items());
    REQUIRE(db.statement_cache_size() == 1);

    db.execute(insert_item(2, "second"));
    REQUIRE(db.statement_cache_size() == 1);

    const auto rows = db.query(select_items());
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::int64_t>(rows[1][0]) == 2);
}

TEST_CASE("statement cache can be disabled without changing results", "[sqlite][cache]")
{
    auto db = make_client();
    db.set_statement_cache_capacity(0);

    db.execute(insert_item(1, "first"));
    db.execute(insert_item(2, "second"));

    REQUIRE(db.statement_cache_size() == 0);

    const auto rows = db.query(select_items());
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::string>(rows[0][1]) == "first");
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
}

TEST_CASE("clearing the statement cache keeps queries correct", "[sqlite][cache]")
{
    auto db = make_client();

    db.execute(insert_item(1, "first"));
    REQUIRE(db.statement_cache_size() == 1);

    db.clear_statement_cache();
    REQUIRE(db.statement_cache_size() == 0);

    db.execute(insert_item(2, "second"));
    const auto rows = db.query(select_items());

    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::int64_t>(rows[1][0]) == 2);
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
}

TEST_CASE("cached statements stay correct inside a transaction", "[sqlite][cache]")
{
    auto db = make_client();

    db.transaction([&] {
        db.execute(insert_item(1, "first"));
        db.execute(insert_item(2, "second"));
    });

    const auto rows = db.query(select_items());
    REQUIRE(rows.size() == 2);
    REQUIRE(std::get<std::string>(rows[1][1]) == "second");
}
