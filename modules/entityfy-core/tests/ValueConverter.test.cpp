#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <variant>
#include <vector>

namespace
{

struct geo_point
{
    double latitude = 0.0;
    double longitude = 0.0;

    friend bool operator==(const geo_point&, const geo_point&) = default;
};

struct [[= entityfy::table{"places"}]] place
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::text]]
    std::string name;

    geo_point position;
};

struct tag_list
{
    std::vector<std::string> tags;

    friend bool operator==(const tag_list&, const tag_list&) = default;
};

struct [[= entityfy::table{"tagged"}]] tagged_place
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::varchar{64}]]
    geo_point coordinates;

    [[= entityfy::json]]
    tag_list labels;

    [[= entityfy::sql_type{"VARCHAR(32)"}]]
    std::string code;
};

} // namespace

template <>
struct entityfy::value_converter<geo_point>
{
    static geo_point to_value(const entityfy::sql_value& raw)
    {
        const auto text = std::get<std::string>(raw);
        const auto separator = text.find(',');

        if(separator == std::string::npos)
        {
            throw std::runtime_error{"entityfy: invalid geo_point payload"};
        }

        geo_point output;
        output.latitude = std::stod(text.substr(0, separator));
        output.longitude = std::stod(text.substr(separator + 1));
        return output;
    }

    static entityfy::sql_value from_value(const geo_point& value)
    {
        return std::format("{:.2f},{:.2f}", value.latitude, value.longitude);
    }
};

template <>
struct entityfy::value_converter<tag_list>
{
    static tag_list to_value(const entityfy::sql_value& raw)
    {
        const auto text = std::get<std::string>(raw);

        tag_list output;
        std::string current;
        bool        quoted = false;

        for(const char character: text)
        {
            if(character == '"')
            {
                quoted = !quoted;
                continue;
            }

            if(character == ',' && !quoted)
            {
                if(!current.empty())
                {
                    output.tags.push_back(current);
                }

                current.clear();
                continue;
            }

            if(character == '[' || character == ']')
            {
                continue;
            }

            current.push_back(character);
        }

        if(!current.empty())
        {
            output.tags.push_back(current);
        }

        return output;
    }

    static entityfy::sql_value from_value(const tag_list& value)
    {
        std::string output = "[";
        for(std::size_t index = 0; index < value.tags.size(); ++index)
        {
            output += index == 0 ? "" : ",";
            output += "\"" + value.tags[index] + "\"";
        }

        output += "]";
        return output;
    }
};

TEST_CASE("value converters persist custom types in a single column", "[core][converter]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<place>();

    const geo_point position{.latitude = 41.01, .longitude = 28.97};

    db.insert(place{.id = 1, .name = "home", .position = position});

    const auto found = db.find<place>(1);
    REQUIRE(found.has_value());
    REQUIRE(found->position == position);
    REQUIRE(found->name == "home");

    const auto rows = db.query(entityfy::statement{
        .sql = "SELECT \"position\" FROM \"places\" WHERE \"id\" = 1"});
    REQUIRE(rows.size() == 1);
    REQUIRE(std::get<std::string>(rows.front().front()) == "41.01,28.97");
}

TEST_CASE("value converter columns default to text and can be narrowed", "[core][converter]")
{
    const auto model = entityfy::describe_model<place>(entityfy::dialect::sqlite);

    bool found_position = false;
    for(const auto& column: model.columns)
    {
        if(column.name == "position")
        {
            found_position = true;
            REQUIRE(column.sql_type == "TEXT");
        }
    }

    REQUIRE(found_position);
}

TEST_CASE("value converter fields accept varchar and json annotations", "[core][converter]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<tagged_place>();

    const auto model = entityfy::describe_model<tagged_place>(entityfy::dialect::sqlite);

    for(const auto& column: model.columns)
    {
        if(column.name == "coordinates")
        {
            REQUIRE(column.sql_type == "VARCHAR(64)");
        }

        if(column.name == "labels")
        {
            REQUIRE(column.sql_type == "TEXT");
        }

        if(column.name == "code")
        {
            REQUIRE(column.sql_type == "VARCHAR(32)");
        }
    }

    const geo_point coordinates{.latitude = 12.5, .longitude = 34.25};
    const tag_list labels{.tags = {"alpha", "beta"}};

    db.insert(tagged_place{.id = 1, .coordinates = coordinates, .labels = labels, .code = "AB"});

    const auto found = db.find<tagged_place>(1);
    REQUIRE(found.has_value());
    REQUIRE(found->coordinates == coordinates);
    REQUIRE(found->labels == labels);
    REQUIRE(found->code == "AB");

    const auto rows = db.query(entityfy::statement{
        .sql = "SELECT \"labels\" FROM \"tagged\" WHERE \"id\" = 1"});
    REQUIRE(rows.size() == 1);
    REQUIRE(std::get<std::string>(rows.front().front()) == "[\"alpha\",\"beta\"]");
}
