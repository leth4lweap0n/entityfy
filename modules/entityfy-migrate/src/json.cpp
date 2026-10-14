#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/util.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace entityfy::migrate
{
namespace
{

using json = nlohmann::json;

json column_to_json(const column& value)
{
    json output;
    output["name"] = value.name;
    output["type"] = value.type;
    output["not_null"] = value.not_null;
    output["primary_key"] = value.primary_key;
    output["auto_increment"] = value.auto_increment;
    output["unique"] = value.unique;
    output["indexed"] = value.indexed;
    output["default_sql"] = value.default_sql;
    output["check_sql"] = value.check_sql;

    if(value.references)
    {
        output["references"] = {
            {"table", value.references->table},
            {"column", value.references->column},
            {"on_delete", value.references->on_delete},
            {"on_update", value.references->on_update},
        };
    }
    else
    {
        output["references"] = nullptr;
    }

    return output;
}

column column_from_json(const json& value)
{
    column output;
    output.name = value.value("name", "");
    output.type = value.value("type", "");
    output.not_null = value.value("not_null", false);
    output.primary_key = value.value("primary_key", false);
    output.auto_increment = value.value("auto_increment", false);
    output.unique = value.value("unique", false);
    output.indexed = value.value("indexed", false);
    output.default_sql = value.value("default_sql", "");
    output.check_sql = value.value("check_sql", "");

    if(value.contains("references") && !value["references"].is_null())
    {
        const auto& source = value["references"];
        output.references = reference{
            .table = source.value("table", ""),
            .column = source.value("column", "id"),
            .on_delete = source.value("on_delete", ""),
            .on_update = source.value("on_update", ""),
        };
    }

    return output;
}

json table_to_json(const table& value)
{
    json output;
    output["name"] = value.name;

    auto columns = json::array();
    for(const auto& current: value.columns)
    {
        columns.push_back(column_to_json(current));
    }
    output["columns"] = std::move(columns);

    auto indexes = json::array();
    for(const auto& current: value.indexes)
    {
        indexes.push_back({
            {"name", current.name},
            {"columns", current.columns},
            {"unique", current.unique},
        });
    }
    output["indexes"] = std::move(indexes);

    auto foreign_keys = json::array();
    for(const auto& current: value.foreign_keys)
    {
        foreign_keys.push_back({
            {"column", current.column},
            {"ref_table", current.ref_table},
            {"ref_column", current.ref_column},
            {"on_delete", current.on_delete},
            {"on_update", current.on_update},
        });
    }
    output["foreign_keys"] = std::move(foreign_keys);

    return output;
}

table table_from_json(const json& value)
{
    table output;
    output.name = value.value("name", "");

    if(value.contains("columns"))
    {
        for(const auto& current: value["columns"])
        {
            output.columns.push_back(column_from_json(current));
        }
    }

    if(value.contains("indexes"))
    {
        for(const auto& current: value["indexes"])
        {
            output.indexes.push_back(index{
                .name = current.value("name", ""),
                .columns = current.value("columns", std::vector<std::string>{}),
                .unique = current.value("unique", false),
            });
        }
    }

    if(value.contains("foreign_keys"))
    {
        for(const auto& current: value["foreign_keys"])
        {
            output.foreign_keys.push_back(foreign_key{
                .column = current.value("column", ""),
                .ref_table = current.value("ref_table", ""),
                .ref_column = current.value("ref_column", "id"),
                .on_delete = current.value("on_delete", ""),
                .on_update = current.value("on_update", ""),
            });
        }
    }

    return output;
}

json schema_to_json_value(const schema& value)
{
    json output;
    output["version"] = 1;

    auto tables = json::array();
    for(const auto& current: value.tables)
    {
        tables.push_back(table_to_json(current));
    }
    output["tables"] = std::move(tables);

    return output;
}

schema schema_from_json_value(const json& value)
{
    schema output;

    if(value.contains("tables"))
    {
        for(const auto& current: value["tables"])
        {
            output.tables.push_back(table_from_json(current));
        }
    }

    return output;
}

} // namespace

std::string schema_to_json(const schema& value)
{
    return schema_to_json_value(value).dump(2);
}

schema schema_from_json(std::string_view text)
{
    return schema_from_json_value(json::parse(text.begin(), text.end()));
}

std::string migration_to_json(const migration_file& value)
{
    json output;
    output["id"] = value.id;
    output["created_at"] = value.created_at;
    output["provider"] = value.provider;
    output["checksum"] = value.checksum;
    output["before"] = schema_to_json_value(value.before);
    output["after"] = schema_to_json_value(value.after);
    return output.dump(2);
}

migration_file migration_from_json(std::string_view text)
{
    const auto parsed = json::parse(text.begin(), text.end());

    migration_file output;
    output.id = parsed.value("id", "");
    output.created_at = parsed.value("created_at", "");
    output.provider = parsed.value("provider", "");
    output.checksum = parsed.value("checksum", "");

    if(parsed.contains("before"))
    {
        output.before = schema_from_json_value(parsed["before"]);
    }

    if(parsed.contains("after"))
    {
        output.after = schema_from_json_value(parsed["after"]);
    }

    return output;
}

void write_file(const std::filesystem::path& path, std::string_view content)
{
    ensure_directory(path.parent_path());

    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if(!stream)
    {
        throw std::runtime_error{"entityfy-migrate: cannot write " + path.string()};
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if(!stream)
    {
        throw std::runtime_error{"entityfy-migrate: failed while writing " + path.string()};
    }
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    if(!stream)
    {
        throw std::runtime_error{"entityfy-migrate: cannot read " + path.string()};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace entityfy::migrate
