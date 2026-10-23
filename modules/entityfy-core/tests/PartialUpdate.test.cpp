#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <string>

namespace
{

struct [[= entityfy::table{"partial_items"}]] partial_item
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string note;
};

} // namespace

TEST_CASE("partial update writes only the named columns", "[sqlite][update]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<partial_item>();

    const auto inserted = db.insert(partial_item{.name = "alpha", .note = "keep me"});
    const auto stored = db.find<partial_item>(inserted.last_insert_id);
    REQUIRE(stored.has_value());

    // The in-memory copy carries a different note that must not be written.
    partial_item changed = *stored;
    changed.name = "beta";
    changed.note = "must not reach the database";

    const auto affected = db.update_partial(changed, &partial_item::name);
    REQUIRE(affected == 1);

    const auto loaded = db.find<partial_item>(stored->id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "beta");
    REQUIRE(loaded->note == "keep me");
}

TEST_CASE("partial update can write several columns", "[sqlite][update]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<partial_item>();

    const auto inserted = db.insert(partial_item{.name = "alpha", .note = "first"});
    const auto stored = db.find<partial_item>(inserted.last_insert_id);
    REQUIRE(stored.has_value());

    partial_item changed = *stored;
    changed.name = "gamma";
    changed.note = "second";

    REQUIRE(db.update_partial(changed, &partial_item::name, &partial_item::note) == 1);

    const auto loaded = db.find<partial_item>(stored->id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "gamma");
    REQUIRE(loaded->note == "second");
}
