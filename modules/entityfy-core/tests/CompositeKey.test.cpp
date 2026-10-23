#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

[[nodiscard]] const entityfy::column_info* find_column_info(const entityfy::table_info& table, std::string_view name)
{
    const auto found = std::ranges::find_if(table.columns, [&](const entityfy::column_info& column) {
        return column.name == name;
    });

    return found == table.columns.end() ? nullptr : &*found;
}

// Two key parts, both explicit: the caller owns the whole identity and the
// table has no auto increment column.
struct [[= entityfy::table{"ck_people"}]] ck_person
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

// A child of the composite parent. `references` names one column, so this is
// the only shape a foreign key to a composite-key table can take today.
struct [[= entityfy::table{"ck_pets"}]] ck_pet
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"ck_people", "id", "CASCADE", "CASCADE"}]]
    std::int64_t person_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

// One non-integral key part.
struct [[= entityfy::table{"ck_slugs"}]] ck_slug
{
    [[= entityfy::primary_key, = entityfy::varchar{64}]]
    std::string slug;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

// A GUID key: std::string storage with the portable UUID column type.
struct [[= entityfy::table{"ck_uuid_events"}]] ck_uuid_event
{
    [[= entityfy::primary_key, = entityfy::uuid]]
    std::string id;

    [[= entityfy::not_null, = entityfy::varchar{120}]]
    std::string name;
};

[[nodiscard]] entityfy::row_key person_key(std::int64_t id, std::string region)
{
    return entityfy::row_key{entityfy::sql_value{id}, entityfy::sql_value{std::move(region)}};
}

[[nodiscard]] entityfy::row_key slug_key(std::string slug)
{
    return entityfy::row_key{entityfy::sql_value{std::move(slug)}};
}

// The helpers throw before touching the database, so the message is collected
// in one place and asserted by the caller.
template <typename Function>
[[nodiscard]] std::string rejection_message(Function&& function)
{
    try
    {
        std::forward<Function>(function)();
    }
    catch(const std::logic_error& error)
    {
        return std::string{error.what()};
    }

    return {};
}

[[nodiscard]] std::vector<std::string> sorted_titles(std::vector<ck_person> people)
{
    std::vector<std::string> titles;
    titles.reserve(people.size());

    for(auto& person: people)
    {
        titles.emplace_back(std::move(person.title));
    }

    std::ranges::sort(titles);
    return titles;
}

} // namespace

TEST_CASE("sqlite composite primary key writes and reads rows by row_key", "[composite][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<ck_person>();

    const auto sqlite_table = db.inspect_table("ck_people");
    REQUIRE(sqlite_table.exists());

    // The pragma reports each column's 1-based position in the key, so
    // introspection keeps the declaration order of the composite key.
    const auto* key_id = find_column_info(sqlite_table, "id");
    REQUIRE(key_id != nullptr);
    REQUIRE(key_id->primary_key);
    REQUIRE(key_id->primary_key_ordinal == 1);

    const auto* key_region = find_column_info(sqlite_table, "region");
    REQUIRE(key_region != nullptr);
    REQUIRE(key_region->primary_key);
    REQUIRE(key_region->primary_key_ordinal == 2);

    const auto* non_key_title = find_column_info(sqlite_table, "title");
    REQUIRE(non_key_title != nullptr);
    REQUIRE_FALSE(non_key_title->primary_key);
    REQUIRE(non_key_title->primary_key_ordinal == 0);

    const auto* non_key_score = find_column_info(sqlite_table, "score");
    REQUIRE(non_key_score != nullptr);
    REQUIRE(non_key_score->primary_key_ordinal == 0);

    const auto key_columns = entityfy::primary_key_columns<ck_person>(db.target_dialect());
    REQUIRE(key_columns.size() == 2);
    REQUIRE(key_columns[0].name == "id");
    REQUIRE(key_columns[1].name == "region");

    // The whole key is explicit, so the descriptor must mark both parts.
    const auto descriptor = db.table<ck_person>().descriptor();
    REQUIRE(descriptor.primary_key_indices.size() == 2);
    REQUIRE(
        entityfy::detail::primary_key_of(ck_person{.id = 5, .region = "eu"}, descriptor) == person_key(5, "eu")
    );

    REQUIRE(db.insert(ck_person{.id = 1, .region = "eu", .title = "alpha", .score = 10}).rows_affected == 1);

    const auto inserted = db.insert_many(std::vector<ck_person>{
        {.id = 2, .region = "eu", .title = "beta", .score = 20},
        {.id = 1, .region = "us", .title = "gamma", .score = 30},
        {.id = 3, .region = "us", .title = "delta", .score = 40},
    });
    REQUIRE(inserted.rows_affected == 3);
    REQUIRE(db.count<ck_person>() == 4);

    // The composite key is the real uniqueness constraint, not one column.
    REQUIRE_THROWS_AS(
        db.insert(ck_person{.id = 1, .region = "eu", .title = "duplicate", .score = 0}),
        entityfy::unique_violation
    );

    const auto eu = db.find<ck_person>(person_key(1, "eu"));
    REQUIRE(eu.has_value());
    REQUIRE(eu->title == "alpha");
    REQUIRE(eu->score == 10);

    // The same id under another region is a different row.
    const auto us = db.find<ck_person>(person_key(1, "us"));
    REQUIRE(us.has_value());
    REQUIRE(us->title == "gamma");

    // A key that shares only one part with a stored row is not found.
    REQUIRE_FALSE(db.find<ck_person>(person_key(1, "apac")).has_value());
    REQUIRE_FALSE(db.find<ck_person>(person_key(9, "eu")).has_value());

    auto updated = *eu;
    updated.title = "alpha-renamed";
    updated.score = 11;
    REQUIRE(db.update(updated) == 1);

    const auto reread = db.find<ck_person>(person_key(1, "eu"));
    REQUIRE(reread.has_value());
    REQUIRE(reread->title == "alpha-renamed");
    REQUIRE(reread->score == 11);

    // The sibling row that shares the id part was not touched.
    const auto sibling = db.find<ck_person>(person_key(1, "us"));
    REQUIRE(sibling.has_value());
    REQUIRE(sibling->title == "gamma");
    REQUIRE(sibling->score == 30);

    REQUIRE(db.remove_by_key<ck_person>(person_key(1, "eu")) == 1);
    REQUIRE(db.count<ck_person>() == 3);
    REQUIRE_FALSE(db.find<ck_person>(person_key(1, "eu")).has_value());

    // Exactly one row went away: the row with the other key part is still there.
    REQUIRE(db.find<ck_person>(person_key(1, "us")).has_value());

    // Removing a key that is already gone affects no rows.
    REQUIRE(db.remove_by_key<ck_person>(person_key(1, "eu")) == 0);

    const auto us_rows = db.find_many<ck_person>(entityfy::where(&ck_person::region).eq(std::string{"us"}));
    REQUIRE(us_rows.size() == 2);
    REQUIRE(sorted_titles(us_rows) == std::vector<std::string>{"delta", "gamma"});

    REQUIRE(db.count<ck_person>(entityfy::where(&ck_person::region).eq(std::string{"eu"})) == 1);
}

TEST_CASE("sqlite string primary key round trips through row_key", "[composite][sqlite][string-key]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<ck_slug>();

    const auto slug_table = db.inspect_table("ck_slugs");
    REQUIRE(slug_table.exists());

    const auto* key_slug = find_column_info(slug_table, "slug");
    REQUIRE(key_slug != nullptr);
    REQUIRE(key_slug->primary_key);
    REQUIRE(key_slug->primary_key_ordinal == 1);

    const auto* non_key_name = find_column_info(slug_table, "name");
    REQUIRE(non_key_name != nullptr);
    REQUIRE(non_key_name->primary_key_ordinal == 0);

    const auto key_columns = entityfy::primary_key_columns<ck_slug>(db.target_dialect());
    REQUIRE(key_columns.size() == 1);
    REQUIRE(key_columns[0].name == "slug");

    REQUIRE(db.insert(ck_slug{.slug = "entityfy", .name = "Entityfy"}).rows_affected == 1);
    REQUIRE_THROWS_AS(db.insert(ck_slug{.slug = "entityfy", .name = "Duplicate"}), entityfy::unique_violation);

    const auto loaded = db.find<ck_slug>(slug_key("entityfy"));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "Entityfy");

    REQUIRE_FALSE(db.find<ck_slug>(slug_key("missing")).has_value());

    // A single-value key is the same key as the one-part row_key.
    const auto by_value = db.find<ck_slug>(std::string{"entityfy"});
    REQUIRE(by_value.has_value());
    REQUIRE(by_value->name == "Entityfy");

    auto renamed = *loaded;
    renamed.name = "Entityfy ORM";
    REQUIRE(db.update(renamed) == 1);

    const auto reread = db.find<ck_slug>(slug_key("entityfy"));
    REQUIRE(reread.has_value());
    REQUIRE(reread->name == "Entityfy ORM");

    REQUIRE(db.remove_by_key<ck_slug>(slug_key("entityfy")) == 1);
    REQUIRE(db.count<ck_slug>() == 0);
    REQUIRE_FALSE(db.find<ck_slug>(slug_key("entityfy")).has_value());
    REQUIRE(db.remove_by_key<ck_slug>(slug_key("entityfy")) == 0);
}

TEST_CASE("sqlite guid primary key round trips through row_key", "[composite][sqlite][uuid-key]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<ck_uuid_event>();

    const auto key_columns = entityfy::primary_key_columns<ck_uuid_event>(db.target_dialect());
    REQUIRE(key_columns.size() == 1);
    REQUIRE(key_columns[0].name == "id");

    const std::string id = "6f1c2c9e-3a6d-4f4e-9f1a-2b7c8d9e0f11";
    REQUIRE(db.insert(ck_uuid_event{.id = id, .name = "created"}).rows_affected == 1);
    REQUIRE_THROWS_AS(
        db.insert(ck_uuid_event{.id = id, .name = "duplicate"}),
        entityfy::unique_violation
    );

    const auto loaded = db.find<ck_uuid_event>(entityfy::row_key{entityfy::sql_value{id}});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "created");

    const auto by_value = db.find<ck_uuid_event>(id);
    REQUIRE(by_value.has_value());

    auto renamed = *loaded;
    renamed.name = "updated";
    REQUIRE(db.update(renamed) == 1);
    REQUIRE(db.find<ck_uuid_event>(id)->name == "updated");

    REQUIRE(db.remove_by_key<ck_uuid_event>(entityfy::row_key{entityfy::sql_value{id}}) == 1);
    REQUIRE_FALSE(db.find<ck_uuid_event>(id).has_value());
}

#ifdef ENTITYFY_ENABLE_POSTGRESQL

TEST_CASE("postgres guid primary key round trips through row_key", "[composite][postgresql][uuid-key]")
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr)
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"ck_uuid_events\""});
    db.migrate<ck_uuid_event>();

    const std::string id = "6f1c2c9e-3a6d-4f4e-9f1a-2b7c8d9e0f12";
    (void)db.remove_by_key<ck_uuid_event>(entityfy::row_key{entityfy::sql_value{id}});

    REQUIRE(db.insert(ck_uuid_event{.id = id, .name = "created"}).rows_affected == 1);
    REQUIRE_THROWS_AS(
        db.insert(ck_uuid_event{.id = id, .name = "duplicate"}),
        entityfy::unique_violation
    );

    const auto loaded = db.find<ck_uuid_event>(id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->name == "created");

    auto renamed = *loaded;
    renamed.name = "updated";
    REQUIRE(db.update(renamed) == 1);
    REQUIRE(db.find<ck_uuid_event>(id)->name == "updated");

    REQUIRE(db.remove_by_key<ck_uuid_event>(entityfy::row_key{entityfy::sql_value{id}}) == 1);
    REQUIRE_FALSE(db.find<ck_uuid_event>(id).has_value());

    // The migration suite inspects this database; leave no table behind.
    db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"ck_uuid_events\""});
}

#endif // ENTITYFY_ENABLE_POSTGRESQL

TEST_CASE("sqlite relationship helpers reject a composite parent key", "[composite][sqlite][relations]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<ck_person>();

    const ck_person parent{.id = 1, .region = "eu", .title = "alpha", .score = 10};
    const ck_pet    child{.id = 1, .person_id = 1, .name = "rex"};

    auto people = db.table<ck_person>();
    auto pets = db.table<ck_pet>();

    const auto has_many_message = rejection_message([&] {
        (void)people.has_many<ck_pet>(parent, &ck_pet::person_id);
    });
    REQUIRE(has_many_message.find("has_many") != std::string::npos);
    REQUIRE(has_many_message.find("single-column primary key") != std::string::npos);

    const auto has_one_message = rejection_message([&] {
        (void)people.has_one<ck_pet>(parent, &ck_pet::person_id);
    });
    REQUIRE(has_one_message.find("has_one") != std::string::npos);
    REQUIRE(has_one_message.find("single-column primary key") != std::string::npos);

    const auto belongs_to_message = rejection_message([&] {
        (void)pets.belongs_to<ck_person>(child, &ck_pet::person_id);
    });
    REQUIRE(belongs_to_message.find("belongs_to") != std::string::npos);
    REQUIRE(belongs_to_message.find("single-column primary key") != std::string::npos);

    // The parent row is untouched by the rejected navigation helpers.
    REQUIRE(db.count<ck_person>() == 0);
}
