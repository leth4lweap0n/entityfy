#include "support/Fixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct coordinates
{
    double latitude = 0.0;
    double longitude = 0.0;
};

struct address
{
    [[= entityfy::varchar{80}, = entityfy::not_null]]
    std::string city;

    [[= entityfy::varchar{16}]]
    std::string postal_code;

    [[= entityfy::owned]]
    coordinates point;

    [[= entityfy::ignore]]
    std::string country;
};

struct [[= entityfy::table{"people"}]] person
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::varchar{120}, = entityfy::not_null]]
    std::string name;

    [[= entityfy::owned, = entityfy::column{"home"}]]
    address home;
};

} // namespace

TEST_CASE("owned fields flatten into prefixed columns", "[sqlite][owned]")
{
    const auto model = entityfy::describe_model<person>(entityfy::dialect::sqlite);

    REQUIRE(model.columns.size() == 6);
    REQUIRE(model.columns[0].name == "id");
    REQUIRE(model.columns[1].name == "name");
    REQUIRE(model.columns[2].name == "home_city");
    REQUIRE(model.columns[3].name == "home_postal_code");
    REQUIRE(model.columns[4].name == "home_point_latitude");
    REQUIRE(model.columns[5].name == "home_point_longitude");

    REQUIRE(model.columns[2].sql_type == "VARCHAR(80)");
    REQUIRE(model.columns[2].flags.not_null);
    REQUIRE(model.columns[3].sql_type == "VARCHAR(16)");
    REQUIRE(model.columns[4].sql_type == "REAL");
}

TEST_CASE("owned fields round trip through sqlite", "[sqlite][owned]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<person>();

    const auto inserted = db.insert<person>({
        .name = "Ada",
        .home = {
            .city = "London",
            .postal_code = "N1",
            .point = {.latitude = 51.5, .longitude = -0.12},
            .country = "not persisted",
        },
    });

    const auto loaded = db.find<person>(inserted.last_insert_id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "Ada");
    REQUIRE(loaded->home.city == "London");
    REQUIRE(loaded->home.postal_code == "N1");
    REQUIRE(loaded->home.point.latitude == 51.5);
    REQUIRE(loaded->home.point.longitude == -0.12);
    REQUIRE(loaded->home.country.empty());

    auto changed = *loaded;
    changed.home.city = "Cambridge";
    changed.home.point.latitude = 52.2;
    REQUIRE(db.update(changed) == 1);

    const auto reloaded = db.find<person>(changed.id);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->home.city == "Cambridge");
    REQUIRE(reloaded->home.postal_code == "N1");
    REQUIRE(reloaded->home.point.latitude == 52.2);
}

TEST_CASE("multi-row inserts and upserts walk owned columns in order", "[sqlite][owned]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<person>();

    const auto inserted = db.insert_many<person>({
        {.name = "Ada", .home = {.city = "London", .postal_code = "N1"}},
        {.name = "Grace", .home = {.city = "Arlington", .postal_code = "22201"}},
    });
    REQUIRE(inserted.rows_affected == 2);
    REQUIRE(db.count<person>() == 2);

    auto rows = db.find_many<person>(entityfy::where(&person::id).gte(0));
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].home.postal_code == "N1");
    REQUIRE(rows[1].home.city == "Arlington");

    rows[1].home.postal_code = "22202";
    const auto upserted = db.upsert(rows[1]);
    REQUIRE(upserted.rows_affected >= 1);

    const auto reloaded = db.find<person>(rows[1].id);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->home.postal_code == "22202");
    REQUIRE(reloaded->name == "Grace");
}

TEST_CASE("naming an owned field directly is rejected", "[sqlite][owned]")
{
    REQUIRE_THROWS_AS(entityfy::column_name(&person::home), std::logic_error);
}
