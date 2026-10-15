#include <entityfy/migrate/scaffold.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace entityfy::migrate
{
namespace
{

constexpr auto cpp_keywords = std::to_array<std::string_view>({
    "alignas", "alignof", "and", "and_eq", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
    "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
    "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "final", "float", "for", "friend", "goto", "if",
    "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor", "xor_eq",
});

bool is_keyword(std::string_view value)
{
    return std::ranges::find(cpp_keywords, value) != cpp_keywords.end();
}

std::string sanitize(std::string_view input)
{
    std::string output;

    for(const char character: input)
    {
        output.push_back(std::isalnum(static_cast<unsigned char>(character)) != 0 ? character : '_');
    }

    if(output.empty() || std::isdigit(static_cast<unsigned char>(output.front())) != 0)
    {
        output.insert(output.begin(), '_');
    }

    if(is_keyword(output))
    {
        output.push_back('_');
    }

    return output;
}

std::string unique_name(std::string base, std::vector<std::string>& used)
{
    std::string candidate = base;
    int suffix = 2;

    while(std::ranges::find(used, candidate) != used.end())
    {
        candidate = base + "_" + std::to_string(suffix);
        ++suffix;
    }

    used.push_back(candidate);
    return candidate;
}

std::string pascal_case(std::string_view input)
{
    std::string output;
    bool capitalize = true;

    for(const char character: input)
    {
        if(std::isalnum(static_cast<unsigned char>(character)) == 0)
        {
            capitalize = true;
            continue;
        }

        output.push_back(capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(character))) : character);
        capitalize = false;
    }

    return output.empty() ? std::string{"Model"} : output;
}

std::string singular(std::string name)
{
    if(name.size() > 1 && name.back() == 's' && name[name.size() - 2] != 's')
    {
        name.pop_back();
    }

    return name;
}

std::string escape_cpp(std::string_view input)
{
    std::string output;

    for(const char character: input)
    {
        if(character == '\\' || character == '"')
        {
            output.push_back('\\');
        }

        output.push_back(character);
    }

    return output;
}

bool parse_varchar(std::string_view type, int& length)
{
    const std::string normalized = normalize_sql(type);

    for(const std::string_view prefix: {std::string_view{"VARCHAR("}, std::string_view{"CHARACTERVARYING("}})
    {
        if(!normalized.starts_with(prefix) || normalized.back() != ')')
        {
            continue;
        }

        const std::string digits =
            normalized.substr(prefix.size(), normalized.size() - prefix.size() - 1);

        if(digits.empty() ||
           !std::ranges::all_of(digits, [](char value) {
               return std::isdigit(static_cast<unsigned char>(value)) != 0;
           }))
        {
            return false;
        }

        length = std::stoi(digits);
        return true;
    }

    return false;
}

struct generated_type
{
    std::string cpp;
    std::string annotation;
    std::string initializer;
};

generated_type type_for(const column& value)
{
    const std::string normalized = normalize_sql(value.type);
    int varchar_length = 0;

    const auto equals = [&](std::string_view candidate) {
        return normalized == candidate;
    };

    const auto starts = [&](std::string_view prefix) {
        return normalized.starts_with(prefix);
    };

    if(equals("BOOLEAN") || equals("BOOL"))
    {
        return {.cpp = "bool", .initializer = " = false"};
    }

    if(equals("SMALLINT") || equals("INT") || equals("INTEGER") || equals("BIGINT") || equals("INT2") ||
       equals("INT4") || equals("INT8") || equals("SERIAL") || equals("BIGSERIAL"))
    {
        return {.cpp = "std::int64_t", .initializer = " = 0"};
    }

    if(equals("REAL") || equals("FLOAT") || equals("FLOAT4") || equals("FLOAT8") || equals("DOUBLE") ||
       equals("DOUBLEPRECISION"))
    {
        return {.cpp = "double", .initializer = " = 0.0"};
    }

    if(starts("DECIMAL(") || starts("NUMERIC("))
    {
        return {.cpp = "double", .initializer = " = 0.0"};
    }

    if(equals("DATE"))
    {
        return {.cpp = "std::chrono::sys_days", .annotation = "= entityfy::date", .initializer = "{}"};
    }

    if(equals("TIME") || equals("TIMEWITHOUTTIMEZONE") || equals("TIMEWITHTIMEZONE"))
    {
        return {.cpp = "std::chrono::seconds", .annotation = "= entityfy::time", .initializer = "{}"};
    }

    if(equals("TIMESTAMP") || equals("TIMESTAMPWITHOUTTIMEZONE") || equals("TIMESTAMPWITHTIMEZONE") ||
       equals("TIMESTAMPTZ") || equals("DATETIME"))
    {
        return {.cpp = "std::chrono::system_clock::time_point", .annotation = "= entityfy::timestamp",
                .initializer = "{}"};
    }

    if(parse_varchar(value.type, varchar_length))
    {
        return {.cpp = "std::string",
                .annotation = "= entityfy::varchar{" + std::to_string(varchar_length) + "}",
                .initializer = "{}"};
    }

    if(equals("UUID"))
    {
        return {.cpp = "std::string", .annotation = "= entityfy::uuid", .initializer = "{}"};
    }

    if(equals("JSON") || equals("JSONB"))
    {
        return {.cpp = "std::string", .annotation = "= entityfy::json", .initializer = "{}"};
    }

    if(equals("BLOB") || equals("BYTEA"))
    {
        return {.cpp = "std::vector<std::byte>", .annotation = "= entityfy::blob", .initializer = "{}"};
    }

    if(equals("TEXT"))
    {
        return {.cpp = "std::string", .annotation = "= entityfy::text", .initializer = "{}"};
    }

    return {.cpp = "std::string", .initializer = "{}"};
}

std::string column_annotations(const table& owner, const column& value)
{
    std::vector<std::string> annotations;

    if(value.primary_key)
    {
        annotations.emplace_back("= entityfy::primary_key");
    }

    if(value.auto_increment)
    {
        annotations.emplace_back("= entityfy::auto_increment");
    }

    const bool unique = std::ranges::any_of(owner.indexes, [&](const index& entry) {
        return entry.unique && entry.columns.size() == 1 && entry.columns.front() == value.name;
    });

    const bool indexed = std::ranges::any_of(owner.indexes, [&](const index& entry) {
        return !entry.unique && entry.columns.size() == 1 && entry.columns.front() == value.name;
    });

    if(unique)
    {
        annotations.emplace_back("= entityfy::unique");
    }

    if(indexed)
    {
        annotations.emplace_back("= entityfy::indexed");
    }

    if(value.not_null && !value.primary_key)
    {
        annotations.emplace_back("= entityfy::not_null");
    }

    if(const generated_type type = type_for(value); !type.annotation.empty())
    {
        annotations.push_back(type.annotation);
    }

    if(!value.default_sql.empty())
    {
        annotations.emplace_back("= entityfy::default_value{\"" + escape_cpp(value.default_sql) + "\"}");
    }

    if(value.references)
    {
        std::string reference = "= entityfy::references{\"" + escape_cpp(value.references->table) + "\", \"" +
                                escape_cpp(value.references->column) + "\"";

        if(!value.references->on_delete.empty())
        {
            reference += ", \"" + escape_cpp(value.references->on_delete) + "\"";
        }

        reference += "}";
        annotations.emplace_back(std::move(reference));
    }

    if(annotations.empty())
    {
        return {};
    }

    std::string output = "[[";
    for(std::size_t index = 0; index < annotations.size(); ++index)
    {
        if(index != 0)
        {
            output += ", ";
        }

        output += annotations[index];
    }

    output += "]]";
    return output;
}

} // namespace

std::string generate_models(const schema& value, std::string_view name_space)
{
    std::ostringstream output;

    output << "#pragma once\n\n"
              "#include <entityfy/entityfy.hpp>\n\n"
              "#include <chrono>\n"
              "#include <cstddef>\n"
              "#include <cstdint>\n"
              "#include <string>\n"
              "#include <vector>\n\n"
              "namespace "
           << sanitize(name_space) << "\n{\n";

    std::vector<std::string> used_structs;

    for(const auto& current: value.tables)
    {
        const std::string struct_name = unique_name(sanitize(pascal_case(singular(current.name))), used_structs);

        output << "\nstruct [[= entityfy::table{\"" << escape_cpp(current.name) << "\"}]] " << struct_name << "\n{\n";

        std::vector<std::string> used_members;

        for(const auto& column: current.columns)
        {
            const auto type = type_for(column);
            const std::string member = unique_name(sanitize(column.name), used_members);
            const std::string annotations = column_annotations(current, column);

            if(!annotations.empty())
            {
                output << "    " << annotations << "\n";
            }

            output << "    " << type.cpp << " " << member << type.initializer << ";\n\n";
        }

        output << "};\n";
    }

    output << "\n} // namespace " << sanitize(name_space) << "\n";
    return output.str();
}

} // namespace entityfy::migrate
