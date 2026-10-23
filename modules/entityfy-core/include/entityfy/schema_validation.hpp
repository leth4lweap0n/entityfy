#pragma once

#include <entityfy/error.hpp>
#include <entityfy/introspection.hpp>
#include <entityfy/schema.hpp>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy
{

enum class schema_issue_kind
{
    missing_table,
    missing_column,
    extra_column,
    type_mismatch,
    nullability_mismatch,
    primary_key_mismatch,
    default_mismatch,
    missing_index,
    index_uniqueness_mismatch,
    missing_foreign_key,
    foreign_key_mismatch,
};

struct schema_issue
{
    schema_issue_kind kind{};
    std::string       object_name;
    std::string       expected;
    std::string       actual;
    std::string       message;
};

struct schema_validation_options
{
    bool allow_extra_columns = false;
    bool check_types = true;
    bool strict_sql_types = false;
    bool check_nullability = true;
    bool check_primary_key = true;
    bool check_defaults = true;
    bool check_indexes = true;
    bool check_foreign_keys = true;
};

struct schema_validation_result
{
    std::vector<schema_issue> issues;

    [[nodiscard]] bool valid() const noexcept
    {
        return issues.empty();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return valid();
    }
};

[[nodiscard]] inline std::string schema_issue_kind_name(schema_issue_kind kind)
{
    switch(kind)
    {
        case schema_issue_kind::missing_table:
            return "missing_table";
        case schema_issue_kind::missing_column:
            return "missing_column";
        case schema_issue_kind::extra_column:
            return "extra_column";
        case schema_issue_kind::type_mismatch:
            return "type_mismatch";
        case schema_issue_kind::nullability_mismatch:
            return "nullability_mismatch";
        case schema_issue_kind::primary_key_mismatch:
            return "primary_key_mismatch";
        case schema_issue_kind::default_mismatch:
            return "default_mismatch";
        case schema_issue_kind::missing_index:
            return "missing_index";
        case schema_issue_kind::index_uniqueness_mismatch:
            return "index_uniqueness_mismatch";
        case schema_issue_kind::missing_foreign_key:
            return "missing_foreign_key";
        case schema_issue_kind::foreign_key_mismatch:
            return "foreign_key_mismatch";
    }

    return "unknown_schema_issue";
}

[[nodiscard]] inline std::string format_schema_issues(const schema_validation_result& result)
{
    std::ostringstream output;
    output << "entityfy schema validation failed with " << result.issues.size() << " issue(s)";

    for(const auto& issue: result.issues)
    {
        output << "\n- " << schema_issue_kind_name(issue.kind) << " `" << issue.object_name << "`: " << issue.message;
        if(!issue.expected.empty() || !issue.actual.empty())
        {
            output << " (expected `" << issue.expected << "`, actual `" << issue.actual << "`)";
        }
    }

    return output.str();
}

class schema_validation_error : public orm_error
{
public:
    explicit schema_validation_error(schema_validation_result result)
        : orm_error{format_schema_issues(result)},
          result_{std::move(result)}
    {
    }

    [[nodiscard]] const schema_validation_result& result() const noexcept
    {
        return result_;
    }

private:
    schema_validation_result result_;
};

namespace detail
{

[[nodiscard]] inline std::string trim_copy(std::string_view input)
{
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    };

    std::size_t begin = 0;
    while(begin < input.size() && is_space(input[begin]))
    {
        ++begin;
    }

    std::size_t end = input.size();
    while(end > begin && is_space(input[end - 1]))
    {
        --end;
    }

    return std::string{input.substr(begin, end - begin)};
}

[[nodiscard]] inline char ascii_upper(char character)
{
    if(character >= 'a' && character <= 'z')
    {
        return static_cast<char>(character - 'a' + 'A');
    }

    return character;
}

[[nodiscard]] inline std::string uppercase_copy(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    for(const char character: input)
    {
        output.push_back(ascii_upper(character));
    }

    return output;
}

[[nodiscard]] inline std::string compact_type_text(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    bool previous_space = false;
    for(const char character: uppercase_copy(trim_copy(input)))
    {
        const bool space = character == ' ' || character == '\t' || character == '\n' || character == '\r';
        if(space)
        {
            if(!previous_space)
            {
                output.push_back(' ');
            }
        }
        else
        {
            output.push_back(character);
        }

        previous_space = space;
    }

    return output;
}

[[nodiscard]] inline std::string strip_type_parameters(std::string text)
{
    const auto open = text.find('(');
    if(open != std::string::npos)
    {
        text.erase(open);
    }

    return trim_copy(text);
}

[[nodiscard]] inline bool contains_text(std::string_view input, std::string_view needle)
{
    return input.find(needle) != std::string_view::npos;
}

[[nodiscard]] inline std::string sql_type_family(std::string_view sql_type)
{
    const auto normalized = strip_type_parameters(compact_type_text(sql_type));

    if(contains_text(normalized, "BIGINT") || contains_text(normalized, "INT8"))
    {
        return "BIGINT";
    }

    if(contains_text(normalized, "INTEGER") || contains_text(normalized, " INT") || normalized == "INT" ||
       contains_text(normalized, "INT4") || contains_text(normalized, "SERIAL"))
    {
        return "INTEGER";
    }

    if(contains_text(normalized, "BOOL"))
    {
        return "BOOLEAN";
    }

    if(contains_text(normalized, "DOUBLE") || contains_text(normalized, "REAL") || contains_text(normalized, "FLOAT"))
    {
        return "FLOAT";
    }

    if(contains_text(normalized, "DECIMAL") || contains_text(normalized, "NUMERIC"))
    {
        return "DECIMAL";
    }

    if(contains_text(normalized, "CHAR") || contains_text(normalized, "TEXT") || contains_text(normalized, "CLOB"))
    {
        return "TEXT";
    }

    if(contains_text(normalized, "TIMESTAMP") || contains_text(normalized, "DATETIME"))
    {
        return "TIMESTAMP";
    }

    if(normalized == "DATE")
    {
        return "DATE";
    }

    if(contains_text(normalized, "TIME"))
    {
        return "TIME";
    }

    if(contains_text(normalized, "BLOB") || contains_text(normalized, "BYTEA") || contains_text(normalized, "BINARY"))
    {
        return "BYTES";
    }

    if(contains_text(normalized, "JSON"))
    {
        return "JSON";
    }

    if(contains_text(normalized, "UUID"))
    {
        return "UUID";
    }

    return normalized;
}

[[nodiscard]] inline bool sql_types_match(std::string_view expected, std::string_view actual, bool strict)
{
    if(strict)
    {
        return compact_type_text(expected) == compact_type_text(actual);
    }

    return sql_type_family(expected) == sql_type_family(actual);
}

[[nodiscard]] inline std::string strip_wrapping_parentheses(std::string input)
{
    input = trim_copy(input);

    while(input.size() >= 2 && input.front() == '(' && input.back() == ')')
    {
        std::size_t depth = 0;
        bool wraps_entire_input = true;

        for(std::size_t index = 0; index < input.size(); ++index)
        {
            if(input[index] == '(')
            {
                ++depth;
            }
            else if(input[index] == ')')
            {
                if(depth == 0)
                {
                    wraps_entire_input = false;
                    break;
                }

                --depth;
                if(depth == 0 && index != input.size() - 1)
                {
                    wraps_entire_input = false;
                    break;
                }
            }
        }

        if(!wraps_entire_input)
        {
            break;
        }

        input = trim_copy(std::string_view{input}.substr(1, input.size() - 2));
    }

    return input;
}

[[nodiscard]] inline std::string normalize_default_sql(std::string_view input)
{
    auto output = strip_wrapping_parentheses(trim_copy(input));

    const auto cast = output.find("::");
    if(cast != std::string::npos)
    {
        output = trim_copy(std::string_view{output}.substr(0, cast));
    }

    const auto upper = compact_type_text(output);
    if(upper == "NOW()")
    {
        return "CURRENT_TIMESTAMP";
    }

    if(!output.empty() && output.front() == '\'')
    {
        return output;
    }

    return upper;
}

[[nodiscard]] inline std::string normalize_action(std::string_view action)
{
    auto normalized = compact_type_text(action);
    if(normalized.empty())
    {
        return "NO ACTION";
    }

    // RESTRICT and NO ACTION differ only in timing details the ORM never uses;
    // MySQL/MariaDB reports them interchangeably.
    if(normalized == "RESTRICT")
    {
        return "NO ACTION";
    }

    return normalized;
}

inline void add_schema_issue(
    schema_validation_result& result,
    schema_issue_kind kind,
    std::string object_name,
    std::string expected,
    std::string actual,
    std::string message
)
{
    result.issues.emplace_back(schema_issue{
        .kind = kind,
        .object_name = std::move(object_name),
        .expected = std::move(expected),
        .actual = std::move(actual),
        .message = std::move(message),
    });
}

[[nodiscard]] inline const column_info* find_column(const table_info& table, std::string_view name)
{
    const auto found = std::ranges::find_if(table.columns, [&](const column_info& column) {
        return column.name == name;
    });

    return found == table.columns.end() ? nullptr : &*found;
}

template <typename Model>
[[nodiscard]] inline const column_descriptor* find_model_column(const model_descriptor<Model>& model, std::string_view name)
{
    const auto found = std::ranges::find_if(model.columns, [&](const column_descriptor& column) {
        return column.name == name;
    });

    return found == model.columns.end() ? nullptr : &*found;
}

[[nodiscard]] inline const index_info* find_single_column_index(
    const table_info& table,
    std::string_view column_name
)
{
    const auto found = std::ranges::find_if(table.indexes, [&](const index_info& index) {
        return index.columns.size() == 1 && index.columns.front() == column_name;
    });

    return found == table.indexes.end() ? nullptr : &*found;
}

[[nodiscard]] inline const foreign_key_info* find_foreign_key(
    const table_info& table,
    std::string_view column_name
)
{
    const auto found = std::ranges::find_if(table.foreign_keys, [&](const foreign_key_info& foreign_key) {
        return foreign_key.column == column_name;
    });

    return found == table.foreign_keys.end() ? nullptr : &*found;
}

} // namespace detail

template <typename Model>
[[nodiscard]] schema_validation_result validate_schema(
    const model_descriptor<Model>& model,
    const table_info& table,
    schema_validation_options options = {}
)
{
    schema_validation_result result;

    // A derived TPH model only owns part of the shared table: the sibling
    // columns belong to the other types in the hierarchy.
    if(!model.type_tag.empty())
    {
        options.allow_extra_columns = true;
    }

    if(!table.exists())
    {
        detail::add_schema_issue(
            result,
            schema_issue_kind::missing_table,
            model.table_name,
            "table exists",
            "table missing",
            "database table does not exist"
        );
        return result;
    }

    for(const auto& expected_column: model.columns)
    {
        if(expected_column.flags.system)
        {
            continue;
        }

        const auto* actual_column = detail::find_column(table, expected_column.name);
        if(actual_column == nullptr)
        {
            detail::add_schema_issue(
                result,
                schema_issue_kind::missing_column,
                expected_column.name,
                expected_column.sql_type,
                "missing",
                "model column is missing in the database table"
            );
            continue;
        }

        if(options.check_types && !detail::sql_types_match(expected_column.sql_type, actual_column->sql_type, options.strict_sql_types))
        {
            detail::add_schema_issue(
                result,
                schema_issue_kind::type_mismatch,
                expected_column.name,
                expected_column.sql_type,
                actual_column->sql_type,
                "column SQL type differs from the model"
            );
        }

        if(options.check_primary_key && expected_column.flags.primary_key != actual_column->primary_key)
        {
            detail::add_schema_issue(
                result,
                schema_issue_kind::primary_key_mismatch,
                expected_column.name,
                expected_column.flags.primary_key ? "primary key" : "not primary key",
                actual_column->primary_key ? "primary key" : "not primary key",
                "column primary-key status differs from the model"
            );
        }

        const bool expected_nullable = expected_column.flags.nullable && !expected_column.flags.primary_key;
        const bool actual_nullable = actual_column->nullable && !actual_column->primary_key;
        if(options.check_nullability && expected_nullable != actual_nullable)
        {
            detail::add_schema_issue(
                result,
                schema_issue_kind::nullability_mismatch,
                expected_column.name,
                expected_nullable ? "nullable" : "not null",
                actual_nullable ? "nullable" : "not null",
                "column nullability differs from the model"
            );
        }

        if(options.check_defaults && !expected_column.flags.auto_increment)
        {
            const auto expected_default = detail::normalize_default_sql(expected_column.default_sql);
            const auto actual_default = detail::normalize_default_sql(actual_column->default_sql);
            if(expected_default != actual_default)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::default_mismatch,
                    expected_column.name,
                    expected_default,
                    actual_default,
                    "column default expression differs from the model"
                );
            }
        }
    }

    if(!options.allow_extra_columns)
    {
        for(const auto& actual_column: table.columns)
        {
            if(detail::find_model_column(model, actual_column.name) == nullptr)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::extra_column,
                    actual_column.name,
                    "not present in model",
                    actual_column.sql_type,
                    "database table contains a column that is not present in the model"
                );
            }
        }
    }

    if(options.check_indexes)
    {
        for(const auto& expected_column: model.columns)
        {
            if(expected_column.flags.system || expected_column.flags.primary_key ||
               (!expected_column.flags.indexed && !expected_column.flags.unique))
            {
                continue;
            }

            const auto* actual_index = detail::find_single_column_index(table, expected_column.name);
            if(actual_index == nullptr)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::missing_index,
                    expected_column.name,
                    expected_column.flags.unique ? "unique index" : "index",
                    "missing",
                    "model expects a single-column index that is missing in the database table"
                );
                continue;
            }

            if(expected_column.flags.unique != actual_index->unique)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::index_uniqueness_mismatch,
                    expected_column.name,
                    expected_column.flags.unique ? "unique index" : "non-unique index",
                    actual_index->unique ? "unique index" : "non-unique index",
                    "index uniqueness differs from the model"
                );
            }
        }
    }

    if(options.check_foreign_keys)
    {
        for(const auto& expected_relation: model.relations)
        {
            const auto* actual_foreign_key = detail::find_foreign_key(table, expected_relation.local_column);
            if(actual_foreign_key == nullptr)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::missing_foreign_key,
                    expected_relation.local_column,
                    expected_relation.referenced_table + "(" + expected_relation.referenced_column + ")",
                    "missing",
                    "model expects a foreign key that is missing in the database table"
                );
                continue;
            }

            const bool target_matches =
                actual_foreign_key->referenced_table == expected_relation.referenced_table &&
                actual_foreign_key->referenced_column == expected_relation.referenced_column;
            const bool action_matches =
                detail::normalize_action(actual_foreign_key->on_delete) == detail::normalize_action(expected_relation.on_delete) &&
                detail::normalize_action(actual_foreign_key->on_update) == detail::normalize_action(expected_relation.on_update);

            if(!target_matches || !action_matches)
            {
                detail::add_schema_issue(
                    result,
                    schema_issue_kind::foreign_key_mismatch,
                    expected_relation.local_column,
                    expected_relation.referenced_table + "(" + expected_relation.referenced_column + ") ON DELETE " +
                        detail::normalize_action(expected_relation.on_delete) + " ON UPDATE " +
                        detail::normalize_action(expected_relation.on_update),
                    actual_foreign_key->referenced_table + "(" + actual_foreign_key->referenced_column + ") ON DELETE " +
                        detail::normalize_action(actual_foreign_key->on_delete) + " ON UPDATE " +
                        detail::normalize_action(actual_foreign_key->on_update),
                    "foreign-key target or action differs from the model"
                );
            }
        }
    }

    return result;
}

template <typename Model>
void require_schema(
    const model_descriptor<Model>& model,
    const table_info& table,
    schema_validation_options options = {}
)
{
    auto result = validate_schema(model, table, options);
    if(!result.valid())
    {
        throw schema_validation_error{std::move(result)};
    }
}

} // namespace entityfy
