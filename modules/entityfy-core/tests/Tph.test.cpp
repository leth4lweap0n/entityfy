#include "support/Fixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace
{

struct [[= entityfy::table{"animals"}]] animal
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::discriminator]]
    std::string kind;

    [[= entityfy::varchar{60}]]
    std::string name;
};

struct [[= entityfy::type_tag{"cat"}]] cat : animal
{
    int lives = 9;
};

struct [[= entityfy::type_tag{"dog"}]] dog : animal
{
    bool good_boy = true;
};

} // namespace

TEST_CASE("derived models share the base table and add their own columns", "[sqlite][tph]")
{
    const auto base = entityfy::describe_model<animal>(entityfy::dialect::sqlite);
    REQUIRE(base.table_name == "animals");
    REQUIRE(base.columns.size() == 3);
    REQUIRE(base.type_tag.empty());
    REQUIRE(base.discriminator_column == "kind");

    const auto derived = entityfy::describe_model<cat>(entityfy::dialect::sqlite);
    REQUIRE(derived.table_name == "animals");
    REQUIRE(derived.type_tag == "cat");
    REQUIRE(derived.discriminator_column == "kind");
    REQUIRE(derived.columns.size() == 4);
    REQUIRE(derived.columns[0].name == "id");
    REQUIRE(derived.columns[1].name == "kind");
    REQUIRE(derived.columns[1].flags.discriminator);
    REQUIRE(derived.columns[2].name == "name");
    REQUIRE(derived.columns[3].name == "lives");
}

TEST_CASE("derived inserts write their discriminator and reads are scoped", "[sqlite][tph]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate_tph<animal, cat, dog>();

    cat tom;
    tom.name = "Tom";
    tom.kind = "dog";  // the constant wins: derived writes stamp the type tag
    const auto cat_id = db.insert(tom).last_insert_id;

    dog rex;
    rex.name = "Rex";
    rex.good_boy = true;
    const auto dog_id = db.insert(rex).last_insert_id;

    REQUIRE(db.count<animal>() == 2);
    REQUIRE(db.count<cat>() == 1);
    REQUIRE(db.count<dog>() == 1);

    const auto cats = db.find_many<cat>();
    REQUIRE(cats.size() == 1);
    REQUIRE(cats[0].name == "Tom");
    REQUIRE(cats[0].lives == 9);
    REQUIRE(cats[0].kind == "cat");

    REQUIRE_FALSE(db.find<cat>(dog_id).has_value());
    REQUIRE_FALSE(db.find<dog>(cat_id).has_value());
    REQUIRE(db.find<animal>(dog_id).has_value());

    REQUIRE_FALSE(db.exists<cat>(entityfy::where(&cat::id).eq(dog_id)));
    REQUIRE(db.exists<dog>(entityfy::where(&dog::id).eq(dog_id)));

    REQUIRE(db.remove_by_id<cat>(dog_id) == 0);
    REQUIRE(db.remove_by_id<cat>(cat_id) == 1);
    REQUIRE(db.count<animal>() == 1);
}

TEST_CASE("derived updates keep the discriminator and stay inside the type", "[sqlite][tph]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate_tph<animal, cat, dog>();

    cat tom;
    tom.name = "Tom";
    const auto cat_id = db.insert(tom).last_insert_id;

    dog rex;
    rex.name = "Rex";
    db.insert(rex);

    auto loaded = db.find<cat>(cat_id);
    REQUIRE(loaded.has_value());
    loaded->name = "Tom Cat";
    loaded->kind = "dog";  // must not be written by an update
    REQUIRE(db.update(*loaded) == 1);

    const auto reloaded = db.find<cat>(cat_id);
    REQUIRE(reloaded.has_value());
    REQUIRE(reloaded->name == "Tom Cat");
    REQUIRE(reloaded->kind == "cat");

    dog rex2;
    rex2.name = "Rex Two";
    rex2.id = db.find_many<dog>()[0].id;
    rex2.kind = "cat";
    REQUIRE(db.upsert(rex2).rows_affected >= 1);
    REQUIRE(db.count<cat>() == 1);
    REQUIRE(db.count<dog>() == 1);
    REQUIRE(db.find<dog>(rex2.id)->kind == "dog");

    REQUIRE(db.delete_many<cat>(entityfy::where(&cat::name).eq("Tom Cat")) == 1);
    REQUIRE(db.count<dog>() == 1);
}

TEST_CASE("multi-row derived inserts stamp every row", "[sqlite][tph]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate_tph<animal, cat, dog>();

    std::vector<cat> cats(2);
    cats[0].name = "Tom";
    cats[1].name = "Felix";

    REQUIRE(db.insert_many(cats).rows_affected == 2);
    REQUIRE(db.count<cat>() == 2);
    REQUIRE(db.count<animal>() == 2);

    for(const auto& row : db.find_many<cat>())
    {
        REQUIRE(row.kind == "cat");
    }
}

TEST_CASE("polymorphic reads return the concrete types", "[sqlite][tph]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate_tph<animal, cat, dog>();

    cat tom;
    tom.name = "Tom";
    db.insert(tom);

    dog rex;
    rex.name = "Rex";
    db.insert(rex);

    animal bird;
    bird.kind = "bird";
    bird.name = "Tweety";
    db.insert(bird);

    const auto rows = entityfy::find_all_types<animal, cat, dog>(db);
    REQUIRE(rows.size() == 2);
    REQUIRE(std::holds_alternative<cat>(rows[0]));
    REQUIRE(std::get<cat>(rows[0]).name == "Tom");
    REQUIRE(std::holds_alternative<dog>(rows[1]));
    REQUIRE(std::get<dog>(rows[1]).name == "Rex");

    REQUIRE(db.count<animal>() == 3);
    REQUIRE(db.find_many<cat>().size() == 1);
}
