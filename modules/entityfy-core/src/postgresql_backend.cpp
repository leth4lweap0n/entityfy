#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/statement.hpp>

#include <tao/pq.hpp>
#include <tao/pq/binary.hpp>
#include <tao/pq/oid.hpp>
#include <tao/pq/result_traits.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

constexpr std::size_t max_pg_parameters = 1024;

struct pg_parameter
{
    bool        null = false;
    std::string text;
};

[[nodiscard]] pg_parameter to_pg_parameter(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> pg_parameter {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                return pg_parameter{.null = true, .text = {}};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return pg_parameter{.null = false, .text = current ? "true" : "false"};
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return pg_parameter{.null = false, .text = current};
            }
            else if constexpr(std::same_as<current_type, bytes>)
            {
                std::string text = "\\x";
                constexpr char hex[] = "0123456789abcdef";
                text.reserve(2 + current.size() * 2);

                for(const auto byte: current)
                {
                    const auto value = static_cast<unsigned char>(byte);
                    text.push_back(hex[value >> 4U]);
                    text.push_back(hex[value & 0x0FU]);
                }

                return pg_parameter{.null = false, .text = std::move(text)};
            }
            else
            {
                return pg_parameter{.null = false, .text = std::to_string(current)};
            }
        },
        value
    );
}

} // namespace

[[nodiscard]] std::string postgres_placeholders(std::string_view sql)
{
    std::string output;
    output.reserve(sql.size());

    enum class state
    {
        normal,
        single_quoted,
        double_quoted,
        line_comment,
        block_comment,
        dollar_quoted,
    };

    const auto is_dollar_tag_character = [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '_';
    };

    const auto dollar_tag_at = [&](std::size_t position) -> std::string {
        if(sql[position] != '$')
        {
            return {};
        }

        std::size_t end = position + 1;
        while(end < sql.size() && is_dollar_tag_character(sql[end]))
        {
            ++end;
        }

        if(end < sql.size() && sql[end] == '$')
        {
            return std::string{sql.substr(position, end - position + 1)};
        }

        return {};
    };

    std::size_t placeholder_index = 1;
    state       parser_state = state::normal;
    std::string dollar_tag;

    for(std::size_t index = 0; index < sql.size(); ++index)
    {
        const char character = sql[index];

        if(parser_state == state::single_quoted)
        {
            output.push_back(character);
            if(character == '\'' && index + 1 < sql.size() && sql[index + 1] == '\'')
            {
                output.push_back(sql[++index]);
            }
            else if(character == '\'')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::double_quoted)
        {
            output.push_back(character);
            if(character == '"' && index + 1 < sql.size() && sql[index + 1] == '"')
            {
                output.push_back(sql[++index]);
            }
            else if(character == '"')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::line_comment)
        {
            output.push_back(character);
            if(character == '\n')
            {
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::block_comment)
        {
            output.push_back(character);
            if(character == '*' && index + 1 < sql.size() && sql[index + 1] == '/')
            {
                output.push_back(sql[++index]);
                parser_state = state::normal;
            }

            continue;
        }

        if(parser_state == state::dollar_quoted)
        {
            if(sql.substr(index, dollar_tag.size()) == std::string_view{dollar_tag})
            {
                output.append(dollar_tag);
                index += dollar_tag.size() - 1;
                dollar_tag.clear();
                parser_state = state::normal;
            }
            else
            {
                output.push_back(character);
            }

            continue;
        }

        if(character == '?')
        {
            output.push_back('$');
            output.append(std::to_string(placeholder_index++));
        }
        else if(character == '\'')
        {
            output.push_back(character);
            parser_state = state::single_quoted;
        }
        else if(character == '"')
        {
            output.push_back(character);
            parser_state = state::double_quoted;
        }
        else if(character == '-' && index + 1 < sql.size() && sql[index + 1] == '-')
        {
            output.push_back(character);
            output.push_back(sql[++index]);
            parser_state = state::line_comment;
        }
        else if(character == '/' && index + 1 < sql.size() && sql[index + 1] == '*')
        {
            output.push_back(character);
            output.push_back(sql[++index]);
            parser_state = state::block_comment;
        }
        else if(character == '$')
        {
            auto tag = dollar_tag_at(index);
            if(tag.empty())
            {
                if(index + 1 < sql.size() && sql[index + 1] >= '1' && sql[index + 1] <= '9')
                {
                    return std::string{sql};
                }

                output.push_back(character);
            }
            else
            {
                output.append(tag);
                index += tag.size() - 1;
                dollar_tag = std::move(tag);
                parser_state = state::dollar_quoted;
            }
        }
        else
        {
            output.push_back(character);
        }
    }

    return output;
}

namespace
{

[[nodiscard]] std::vector<pg_parameter> to_pg_parameters(const std::vector<sql_value>& values)
{
    std::vector<pg_parameter> output;
    output.reserve(values.size());

    for(const auto& value: values)
    {
        output.emplace_back(to_pg_parameter(value));
    }

    return output;
}

} // namespace

} // namespace entityfy::detail

namespace tao::pq
{

template <>
struct parameter_traits<entityfy::detail::pg_parameter>
{
    const entityfy::detail::pg_parameter& value_ref;

    explicit parameter_traits(const entityfy::detail::pg_parameter& value) noexcept
        : value_ref{value}
    {
    }

    static constexpr std::size_t columns = 1;
    static constexpr bool        self_contained = false;

    template <std::size_t>
    [[nodiscard]] static constexpr auto type() noexcept -> oid
    {
        return oid::invalid;
    }

    template <std::size_t>
    [[nodiscard]] auto value() const noexcept -> const char*
    {
        return value_ref.null ? nullptr : value_ref.text.c_str();
    }

    template <std::size_t>
    [[nodiscard]] auto length() const noexcept -> int
    {
        return value_ref.null ? 0 : static_cast<int>(value_ref.text.size());
    }

    template <std::size_t>
    [[nodiscard]] static constexpr auto format() noexcept -> int
    {
        return 0;
    }

    template <std::size_t>
    void element(std::string& output) const
    {
        output.append(value_ref.text);
    }

    template <std::size_t>
    void copy_to(std::string& output) const
    {
        output.append(value_ref.text);
    }
};

} // namespace tao::pq

namespace entityfy::detail
{
namespace
{

template <std::size_t Max>
void bind_parameters(tao::pq::parameter<Max>& parameters, const std::vector<pg_parameter>& values)
{
    if(values.size() > Max)
    {
        throw std::invalid_argument{"entityfy PostgreSQL backend supports at most 1024 bound parameters per statement"};
    }

    for(const auto& value: values)
    {
        parameters.bind(value);
    }
}

} // namespace

[[nodiscard]] sql_value postgresql_text_value_to_sql_value(const char* value, Oid type_oid)
{
    if(type_oid == static_cast<Oid>(tao::pq::oid::bytea))
    {
        return tao::pq::result_traits<tao::pq::binary>::from(value);
    }

    return std::string{value};
}

class postgresql_backend final : public backend
{
public:
    explicit postgresql_backend(std::string connection_info)
        : connection_{tao::pq::connection::create(std::move(connection_info))}
    {
    }

    [[nodiscard]] dialect target_dialect() const noexcept override
    {
        return dialect::postgresql;
    }

    execution_result execute(const entityfy::statement& input) override
    {
        const std::lock_guard lock{mutex_};

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            tao::pq::parameter<max_pg_parameters> bound;
            bind_parameters(bound, parameters);
            auto result = connection_->execute(sql, bound);

            return execution_result{
                .rows_affected = result.has_rows_affected() ? result.rows_affected() : 0,
            };
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<row> query(const entityfy::statement& input) override
    {
        const std::lock_guard lock{mutex_};

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            auto sql = postgres_placeholders(input.sql);
            auto parameters = to_pg_parameters(input.binds);
            tao::pq::parameter<max_pg_parameters> bound;
            bind_parameters(bound, parameters);
            auto result = connection_->execute(sql, bound);

            std::vector<row> rows;
            rows.reserve(result.size());
            const auto* raw_result = result.underlying_raw_ptr();

            for(const auto& pg_row: result)
            {
                row output;
                output.reserve(pg_row.columns());

                for(std::size_t index = 0; index < pg_row.columns(); ++index)
                {
                    const auto field = pg_row[index];
                    if(field.is_null())
                    {
                        output.emplace_back(std::monostate{});
                    }
                    else
                    {
                        const auto column = static_cast<int>(index);
                        output.emplace_back(postgresql_text_value_to_sql_value(
                            field.get(),
                            PQftype(raw_result, column)
                        ));
                    }
                }

                rows.emplace_back(std::move(output));
            }

            return rows;
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "query", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<std::string> table_columns(std::string_view table) override
    {
        statement input;
        input.sql =
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = ? "
            "ORDER BY ordinal_position";
        input.binds.emplace_back(std::string{table});

        std::vector<std::string> output;
        for(const auto& result_row: query(input))
        {
            if(!result_row.empty())
            {
                output.emplace_back(value_to_string(result_row.front()));
            }
        }

        return output;
    }

    [[nodiscard]] table_info inspect_table(std::string_view table) override
    {
        table_info output;
        output.name = std::string{table};

        statement columns_statement;
        columns_statement.sql =
            "SELECT column_name, data_type, is_nullable, column_default "
            "FROM information_schema.columns "
            "WHERE table_schema = current_schema() AND table_name = ? "
            "ORDER BY ordinal_position";
        columns_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(columns_statement))
        {
            if(result_row.size() < 4)
            {
                continue;
            }

            output.columns.emplace_back(column_info{
                .name = value_to_string(result_row[0]),
                .sql_type = value_to_string(result_row[1]),
                .nullable = value_to_string(result_row[2]) == "YES",
                .primary_key = false,
                .default_sql = value_to_string(result_row[3]),
            });
        }

        statement primary_key_statement;
        primary_key_statement.sql =
            "SELECT kcu.column_name "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "WHERE tc.constraint_type = 'PRIMARY KEY' "
            "AND tc.table_schema = current_schema() AND tc.table_name = ?";
        primary_key_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(primary_key_statement))
        {
            if(result_row.empty())
            {
                continue;
            }

            const auto primary_key_name = value_to_string(result_row.front());
            for(auto& column: output.columns)
            {
                if(column.name == primary_key_name)
                {
                    column.primary_key = true;
                }
            }
        }

        statement indexes_statement;
        indexes_statement.sql =
            "SELECT i.relname, ix.indisunique, string_agg(a.attname, ',' ORDER BY keys.ordinality) "
            "FROM pg_class t "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_index ix ON t.oid = ix.indrelid "
            "JOIN pg_class i ON i.oid = ix.indexrelid "
            "JOIN unnest(ix.indkey) WITH ORDINALITY AS keys(attnum, ordinality) ON true "
            "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = keys.attnum "
            "WHERE n.nspname = current_schema() AND t.relname = ? "
            "GROUP BY i.relname, ix.indisunique";
        indexes_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(indexes_statement))
        {
            if(result_row.size() < 3)
            {
                continue;
            }

            index_info index{
                .name = value_to_string(result_row[0]),
                .unique = value_to_string(result_row[1]) == "t" || value_to_string(result_row[1]) == "true",
            };

            std::string columns = value_to_string(result_row[2]);
            std::size_t offset = 0;
            while(offset <= columns.size())
            {
                const auto comma = columns.find(',', offset);
                index.columns.emplace_back(columns.substr(offset, comma == std::string::npos ? std::string::npos : comma - offset));
                if(comma == std::string::npos)
                {
                    break;
                }

                offset = comma + 1;
            }

            output.indexes.emplace_back(std::move(index));
        }

        statement foreign_keys_statement;
        foreign_keys_statement.sql =
            "SELECT kcu.column_name, ccu.table_name, ccu.column_name, rc.update_rule, rc.delete_rule "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "JOIN information_schema.constraint_column_usage ccu "
            "ON ccu.constraint_name = tc.constraint_name AND ccu.table_schema = tc.table_schema "
            "JOIN information_schema.referential_constraints rc "
            "ON rc.constraint_name = tc.constraint_name AND rc.constraint_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' "
            "AND tc.table_schema = current_schema() AND tc.table_name = ?";
        foreign_keys_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(foreign_keys_statement))
        {
            if(result_row.size() < 5)
            {
                continue;
            }

            output.foreign_keys.emplace_back(foreign_key_info{
                .column = value_to_string(result_row[0]),
                .referenced_table = value_to_string(result_row[1]),
                .referenced_column = value_to_string(result_row[2]),
                .on_update = value_to_string(result_row[3]),
                .on_delete = value_to_string(result_row[4]),
            });
        }

        return output;
    }

    // The PostgreSQL backend talks to libpq through taocpp::pq, which does not
    // expose reusable prepared statements. The cache API stays available so
    // that code stays portable, but the capacity stays zero.
    void set_statement_cache_capacity(std::size_t capacity) override
    {
        (void)capacity;
    }

    [[nodiscard]] std::size_t statement_cache_capacity() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t statement_cache_size() const noexcept override
    {
        return 0;
    }

    void clear_statement_cache() override
    {
    }

    void set_batch_parameter_limit(std::size_t limit) override
    {
        std::lock_guard lock{mutex_};
        batch_parameter_limit_ = limit == 0 ? 1 : limit;
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return batch_parameter_limit_;
    }

    void set_logger(std::function<void(std::string_view)> logger) override
    {
        std::lock_guard lock{mutex_};
        logger_ = std::move(logger);
    }

private:
    std::shared_ptr<tao::pq::connection> connection_;
    mutable std::recursive_mutex           mutex_;
    std::size_t                            batch_parameter_limit_ = max_pg_parameters;
    std::function<void(std::string_view)>  logger_;
};

std::unique_ptr<backend> make_postgresql_backend(std::string connection_info)
{
    return std::make_unique<postgresql_backend>(std::move(connection_info));
}

} // namespace entityfy::detail
