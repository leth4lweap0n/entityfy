#include <entityfy/migrate/schema.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace entityfy::migrate
{

const table* find_table(const schema& value, std::string_view name)
{
    const auto found = std::ranges::find_if(value.tables, [&](const table& candidate) {
        return candidate.name == name;
    });

    return found == value.tables.end() ? nullptr : &*found;
}

table* find_table(schema& value, std::string_view name)
{
    const auto found = std::ranges::find_if(value.tables, [&](const table& candidate) {
        return candidate.name == name;
    });

    return found == value.tables.end() ? nullptr : &*found;
}

const column* find_column(const table& value, std::string_view name)
{
    const auto found = std::ranges::find_if(value.columns, [&](const column& candidate) {
        return candidate.name == name;
    });

    return found == value.columns.end() ? nullptr : &*found;
}

column* find_column(table& value, std::string_view name)
{
    const auto found = std::ranges::find_if(value.columns, [&](const column& candidate) {
        return candidate.name == name;
    });

    return found == value.columns.end() ? nullptr : &*found;
}

const index* find_index(const table& value, std::string_view name)
{
    const auto found = std::ranges::find_if(value.indexes, [&](const index& candidate) {
        return candidate.name == name;
    });

    return found == value.indexes.end() ? nullptr : &*found;
}

std::string normalize_sql(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    bool quoted = false;
    for(const char character: input)
    {
        if(character == '\'')
        {
            quoted = !quoted;
            output.push_back(character);
            continue;
        }

        if(quoted)
        {
            output.push_back(character);
            continue;
        }

        if(std::isspace(static_cast<unsigned char>(character)))
        {
            continue;
        }

        output.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }

    return output;
}

bool same_type(std::string_view left, std::string_view right)
{
    return normalize_sql(left) == normalize_sql(right);
}

namespace
{

std::string trim_copy(std::string_view input)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    if(first == std::string_view::npos)
    {
        return {};
    }

    const auto last = input.find_last_not_of(" \t\r\n");
    return std::string{input.substr(first, last - first + 1)};
}

std::string collapse_space(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    bool quoted = false;
    bool pending_space = false;

    for(const char character: input)
    {
        if(character == '\'')
        {
            quoted = !quoted;
            output.push_back(character);
            pending_space = false;
            continue;
        }

        if(!quoted && std::isspace(static_cast<unsigned char>(character)))
        {
            pending_space = !output.empty();
            continue;
        }

        if(pending_space)
        {
            output.push_back(' ');
            pending_space = false;
        }

        output.push_back(character);
    }

    return output;
}

bool same_reference(const column& left, const column& right)
{
    if(left.references.has_value() != right.references.has_value())
    {
        return false;
    }

    if(!left.references)
    {
        return true;
    }

    const auto& a = *left.references;
    const auto& b = *right.references;
    return a.table == b.table && a.column == b.column && normalize_sql(a.on_delete) == normalize_sql(b.on_delete) &&
           normalize_sql(a.on_update) == normalize_sql(b.on_update);
}

} // namespace

bool same_default(std::string_view left, std::string_view right)
{
    return collapse_space(trim_copy(left)) == collapse_space(trim_copy(right));
}

bool same_schema(const schema& left, const schema& right)
{
    if(left.tables.size() != right.tables.size())
    {
        return false;
    }

    for(const auto& left_table: left.tables)
    {
        const table* right_table = find_table(right, left_table.name);
        if(right_table == nullptr)
        {
            return false;
        }

        if(left_table.columns.size() != right_table->columns.size())
        {
            return false;
        }

        for(const auto& left_column: left_table.columns)
        {
            const column* right_column = find_column(*right_table, left_column.name);
            if(right_column == nullptr)
            {
                return false;
            }

            if(!same_type(left_column.type, right_column->type) ||
               left_column.not_null != right_column->not_null ||
               left_column.primary_key != right_column->primary_key ||
               left_column.auto_increment != right_column->auto_increment ||
               !same_default(left_column.default_sql, right_column->default_sql) ||
               normalize_sql(left_column.check_sql) != normalize_sql(right_column->check_sql) ||
               !same_reference(left_column, *right_column))
            {
                return false;
            }
        }

        if(left_table.indexes.size() != right_table->indexes.size())
        {
            return false;
        }

        for(const auto& left_index: left_table.indexes)
        {
            const index* right_index = find_index(*right_table, left_index.name);
            if(right_index == nullptr || right_index->unique != left_index.unique ||
               right_index->columns != left_index.columns)
            {
                return false;
            }
        }

        if(left_table.foreign_keys.size() != right_table->foreign_keys.size())
        {
            return false;
        }

        for(const auto& left_fk: left_table.foreign_keys)
        {
            const auto found = std::ranges::find_if(right_table->foreign_keys, [&](const foreign_key& candidate) {
                return candidate.column == left_fk.column && candidate.ref_table == left_fk.ref_table &&
                       candidate.ref_column == left_fk.ref_column &&
                       normalize_sql(candidate.on_delete) == normalize_sql(left_fk.on_delete) &&
                       normalize_sql(candidate.on_update) == normalize_sql(left_fk.on_update);
            });

            if(found == right_table->foreign_keys.end())
            {
                return false;
            }
        }
    }

    return true;
}

void sort_schema(schema& value)
{
    std::ranges::sort(value.tables, [](const table& left, const table& right) {
        return left.name < right.name;
    });

    for(auto& current: value.tables)
    {
        std::ranges::sort(current.indexes, [](const index& left, const index& right) {
            return left.name < right.name;
        });

        std::ranges::sort(current.foreign_keys, [](const foreign_key& left, const foreign_key& right) {
            return left.column < right.column;
        });
    }
}

std::string quote_ident(std::string_view identifier)
{
    std::string output;
    output.reserve(identifier.size() + 2);
    output.push_back('"');

    for(const char character: identifier)
    {
        if(character == '"')
        {
            output.push_back('"');
        }

        output.push_back(character);
    }

    output.push_back('"');
    return output;
}

std::string quote_literal(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('\'');

    for(const char character: value)
    {
        if(character == '\'')
        {
            output.push_back('\'');
        }

        output.push_back(character);
    }

    output.push_back('\'');
    return output;
}

} // namespace entityfy::migrate
