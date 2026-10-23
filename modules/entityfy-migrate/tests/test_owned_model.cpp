#include "harness.hpp"

#include <entityfy/migrate/model.hpp>

#include <cstdint>
#include <string>

namespace
{

struct address
{
    [[= entityfy::varchar{80}]] std::string city;
    [[= entityfy::varchar{16}]] std::string postal_code;
};

struct [[= entityfy::table{"people"}]] person
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::owned]]
    address home;
};

} // namespace

entityfy_test::status test_owned_model()
{
    const auto table = entityfy::migrate::table_from_model<person>(entityfy::dialect::sqlite);

    require(table.columns.size() == 3, "owned model flattens to three columns");
    require(table.columns[0].name == "id" && table.columns[0].auto_increment, "owned model primary key");
    require(table.columns[1].name == "home_city" && table.columns[1].type == "VARCHAR(80)",
            "owned model first leaf column");
    require(table.columns[2].name == "home_postal_code" && table.columns[2].type == "VARCHAR(16)",
            "owned model second leaf column");

    const auto postgres = entityfy::migrate::table_from_model<person>(entityfy::dialect::postgresql);
    require(postgres.columns[1].type == "VARCHAR(80)", "owned model keeps nested annotations on PostgreSQL");

    return entityfy_test::status::passed;
}
