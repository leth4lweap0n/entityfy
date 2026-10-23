#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/statement.hpp>

#include <tao/pq.hpp>
#include <tao/pq/binary.hpp>
#include <tao/pq/oid.hpp>
#include <tao/pq/pipeline.hpp>
#include <tao/pq/result_status.hpp>
#include <tao/pq/result_traits.hpp>

#include <cctype>
#include <cstdint>
#include <functional>
#include <set>
#include <list>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

constexpr std::size_t max_pg_parameters = 1024;

// taopq turns PostgreSQL SQLSTATE codes into typed exceptions; the two that a
// retry can resolve get a dedicated entityfy type, everything else keeps the
// message based classification.
[[noreturn]] void throw_pg_error(std::string_view operation,
                                 const std::string& sql,
                                 const tao::pq::sql_error& error)
{
    auto message = sql_context_message("PostgreSQL", operation, sql, error.what());

    if(error.sqlstate == "40001")
    {
        throw serialization_failure{std::move(message), sql};
    }

    if(error.sqlstate == "40P01")
    {
        throw deadlock_detected{std::move(message), sql};
    }

    throw_classified_sql_error("PostgreSQL", operation, sql, error.what());
}

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
        return execute_locked(input);
    }

    // Batched writes use libpq's pipeline mode: every statement is sent before
    // any result is awaited, so a multi-chunk insert costs one round trip.
    [[nodiscard]] std::vector<execution_result> execute_batch(
        const std::vector<entityfy::statement>& statements) override
    {
        const std::lock_guard lock{mutex_};

        bool pipeline_ready = statements.size() > 1;

        for(const auto& entry: statements)
        {
            const auto sql = postgres_placeholders(entry.sql);

            if(is_ddl(sql) || !is_preparable(sql))
            {
                pipeline_ready = false;
                break;
            }
        }

        if(!pipeline_ready)
        {
            std::vector<execution_result> output;
            output.reserve(statements.size());

            for(const auto& entry: statements)
            {
                output.push_back(execute_locked(entry));
            }

            return output;
        }

        try
        {
            std::vector<std::string> sqls;
            sqls.reserve(statements.size());

            for(const auto& entry: statements)
            {
                sqls.push_back(postgres_placeholders(entry.sql));
            }

            // Prepared plans are used only when every distinct text fits in the
            // cache: preparing more would evict an earlier chunk's plan and the
            // pipeline would then send a name the server no longer knows.
            std::vector<std::string> prepared_names(statements.size());
            bool use_prepared = capacity_ != 0;

            if(use_prepared)
            {
                std::set<std::string> distinct{sqls.begin(), sqls.end()};
                use_prepared = distinct.size() <= capacity_;

                if(use_prepared)
                {
                    for(std::size_t index = 0; index < statements.size(); ++index)
                    {
                        prepared_names[index] = statement_name_for(sqls[index]);
                    }
                }
            }

            auto pipe = connection_->pipeline();

            try
            {
                for(std::size_t index = 0; index < statements.size(); ++index)
                {
                    if(logger_)
                    {
                        logger_(statements[index].sql);
                    }

                    auto parameters = to_pg_parameters(statements[index].binds);
                    tao::pq::parameter<max_pg_parameters> bound;
                    bind_parameters(bound, parameters);

                    const std::string& prepared = prepared_names[index];
                    const auto         cached = index_.find(sqls[index]);
                    const bool         usable =
                        !prepared.empty() && cached != index_.end() && cached->second->second == prepared;

                    pipe->send(usable ? prepared : sqls[index], bound);
                }

                pipe->sync();

                std::vector<execution_result> output;
                output.reserve(statements.size());

                for(std::size_t index = 0; index < statements.size(); ++index)
                {
                    auto result = pipe->get_result();
                    output.push_back(execution_result{
                        .rows_affected = result.has_rows_affected() ? result.rows_affected() : 0,
                    });
                }

                pipe->consume_sync();
                return output;
            }
            catch(...)
            {
                // A failed statement aborts the rest of the pipeline. The
                // remaining results (and the sync) have to be consumed before
                // the connection can leave pipeline mode and be used again.
                try
                {
                    while(true)
                    {
                        auto result = pipe->get_result();
                        if(result.status() == tao::pq::result_status::pipeline_sync)
                        {
                            break;
                        }
                    }
                }
                catch(...)
                {
                }

                throw;
            }
        }
        catch(const tao::pq::sql_error& error)
        {
            throw_pg_error("execute batch", statements.front().sql, error);
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute batch", statements.front().sql, error.what());
        }
    }

private:
    execution_result execute_locked(const entityfy::statement& input)
    {
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

            const bool ddl = is_ddl(sql);
            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

            // A prepared plan captures the schema at prepare time; after DDL the
            // cached plans may refer to a dropped or altered relation.
            if(ddl)
            {
                clear_statement_cache_locked();
            }

            return execution_result{
                .rows_affected = result.has_rows_affected() ? result.rows_affected() : 0,
            };
        }
        catch(const tao::pq::sql_error& error)
        {
            throw_pg_error("execute", input.sql, error);
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "execute", input.sql, error.what());
        }
    }

public:
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

            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

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
        catch(const tao::pq::sql_error& error)
        {
            throw_pg_error("query", input.sql, error);
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("PostgreSQL", "query", input.sql, error.what());
        }
    }

    void query_each(const entityfy::statement& input,
                    const std::function<bool(const row&)>& consumer) override
    {
        const std::lock_guard lock{mutex_};
        bool in_consumer = false;

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

            const std::string prepared = statement_name_for(sql);
            auto result = run_statement(sql, prepared, bound);

            const auto* raw_result = result.underlying_raw_ptr();
            row output;

            for(const auto& pg_row: result)
            {
                output.clear();
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
                        output.emplace_back(postgresql_text_value_to_sql_value(
                            field.get(),
                            PQftype(raw_result, static_cast<int>(index))
                        ));
                    }
                }

                in_consumer = true;
                const bool keep = consumer(output);
                in_consumer = false;
                if(!keep)
                {
                    break;
                }
            }
        }
        catch(const std::exception& error)
        {
            if(in_consumer)
            {
                throw;
            }

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

        // The ordered key set lives on the primary key index: indkey lists the
        // key attributes in key order, so unnest with ordinality gives the
        // position of every key column.
        statement primary_key_order_statement;
        primary_key_order_statement.sql =
            "SELECT a.attname, keys.ordinality "
            "FROM pg_class t "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "JOIN pg_index ix ON t.oid = ix.indrelid AND ix.indisprimary "
            "JOIN unnest(ix.indkey) WITH ORDINALITY AS keys(attnum, ordinality) ON true "
            "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = keys.attnum "
            "WHERE n.nspname = current_schema() AND t.relname = ? "
            "ORDER BY keys.ordinality";
        primary_key_order_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(primary_key_order_statement))
        {
            if(result_row.size() < 2)
            {
                continue;
            }

            const auto primary_key_name = value_to_string(result_row[0]);
            const auto primary_key_ordinal = static_cast<int>(value_to_i64(result_row[1]));

            for(auto& column: output.columns)
            {
                if(column.name == primary_key_name)
                {
                    column.primary_key_ordinal = primary_key_ordinal;
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

    // Named server side prepared statements, kept in an LRU like the SQLite
    // backend. taocpp::pq sends PQsendQueryPrepared once a name is prepared,
    // so a cache hit skips parsing and planning on the server.
    void set_statement_cache_capacity(std::size_t capacity) override
    {
        const std::lock_guard lock{mutex_};
        capacity_ = capacity;

        if(capacity_ == 0)
        {
            clear_statement_cache_locked();
            return;
        }

        while(cache_.size() > capacity_)
        {
            deallocate_locked(cache_.back().second);
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }
    }

    [[nodiscard]] std::size_t statement_cache_capacity() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return capacity_;
    }

    [[nodiscard]] std::size_t statement_cache_size() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return cache_.size();
    }

    void clear_statement_cache() override
    {
        const std::lock_guard lock{mutex_};
        clear_statement_cache_locked();
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
    // PREPARE accepts SELECT, INSERT, UPDATE, DELETE, VALUES and WITH; other
    // statements (DDL, SET, COPY, ...) go through the plain execution path.
    // Multi statement input is never prepared either.
    [[nodiscard]] static bool is_preparable(std::string_view sql)
    {
        const auto begin = sql.find_first_not_of(" \t\r\n");
        if(begin == std::string_view::npos)
        {
            return false;
        }

        if(const auto semicolon = sql.find(';'); semicolon != std::string_view::npos)
        {
            if(sql.substr(semicolon + 1).find_first_not_of(" \t\r\n;") != std::string_view::npos)
            {
                return false;
            }
        }

        std::string keyword;
        for(std::size_t index = begin; index < sql.size(); ++index)
        {
            const auto character = static_cast<unsigned char>(sql[index]);
            if(!std::isalpha(character))
            {
                break;
            }

            keyword.push_back(static_cast<char>(std::toupper(character)));
        }

        return keyword == "SELECT" || keyword == "INSERT" || keyword == "UPDATE" || keyword == "DELETE" ||
               keyword == "VALUES" || keyword == "WITH";
    }

    [[nodiscard]] static bool is_ddl(std::string_view sql)
    {
        const auto begin = sql.find_first_not_of(" \t\r\n");
        if(begin == std::string_view::npos)
        {
            return false;
        }

        std::string keyword;
        for(std::size_t index = begin; index < sql.size(); ++index)
        {
            const auto character = static_cast<unsigned char>(sql[index]);
            if(!std::isalpha(character))
            {
                break;
            }

            keyword.push_back(static_cast<char>(std::toupper(character)));
        }

        return keyword == "CREATE" || keyword == "ALTER" || keyword == "DROP" || keyword == "TRUNCATE" ||
               keyword == "RENAME";
    }

    // Returns the prepared statement name for the SQL, or nullptr when the
    // statement should run unprepared. Caller holds the mutex.
    // Empty string when the statement runs unprepared.
    [[nodiscard]] std::string statement_name_for(const std::string& sql)
    {
        if(capacity_ == 0 || !is_preparable(sql))
        {
            return {};
        }

        if(const auto found = index_.find(sql); found != index_.end())
        {
            cache_.splice(cache_.begin(), cache_, found->second);
            return cache_.front().second;
        }

        const auto name = "entityfy_stmt_" + std::to_string(next_statement_id_++);

        try
        {
            connection_->prepare(name, sql);
        }
        catch(const std::exception&)
        {
            // Not preparable in this server: keep using the plain path instead
            // of failing the statement.
            return {};
        }

        cache_.emplace_front(sql, name);
        index_[sql] = cache_.begin();

        while(cache_.size() > capacity_)
        {
            deallocate_locked(cache_.back().second);
            index_.erase(cache_.back().first);
            cache_.pop_back();
        }

        return cache_.front().second;
    }

    // Runs a statement, preferring the prepared plan. A prepared statement can
    // disappear underneath us (PREPARE is undone when the transaction that ran
    // it rolls back), so a failed prepared call falls back to the plain path
    // and forgets the plan instead of surfacing a confusing error.
    [[nodiscard]] tao::pq::result run_statement(const std::string& sql,
                                                const std::string& prepared,
                                                tao::pq::parameter<max_pg_parameters>& bound)
    {
        if(prepared.empty())
        {
            return connection_->execute(sql, bound);
        }

        try
        {
            return connection_->execute(prepared, bound);
        }
        catch(const std::exception&)
        {
            forget_statement(sql);
            return connection_->execute(sql, bound);
        }
    }

    void forget_statement(const std::string& sql)
    {
        const auto found = index_.find(sql);
        if(found == index_.end())
        {
            return;
        }

        deallocate_locked(found->second->second);
        cache_.erase(found->second);
        index_.erase(found);
    }

    void deallocate_locked(const std::string& name) noexcept
    {
        try
        {
            connection_->deallocate(name);
        }
        catch(const std::exception&)
        {
        }
    }

    void clear_statement_cache_locked() noexcept
    {
        for(const auto& entry: cache_)
        {
            deallocate_locked(entry.second);
        }

        cache_.clear();
        index_.clear();
    }

    std::shared_ptr<tao::pq::connection> connection_;
    mutable std::recursive_mutex           mutex_;
    std::size_t                            batch_parameter_limit_ = max_pg_parameters;
    std::size_t                            capacity_ = 32;
    std::uint64_t                          next_statement_id_ = 1;
    std::list<std::pair<std::string, std::string>> cache_;
    std::unordered_map<std::string, decltype(cache_.begin())> index_;
    std::function<void(std::string_view)>  logger_;
};

std::unique_ptr<backend> make_postgresql_backend(std::string connection_info)
{
    return std::make_unique<postgresql_backend>(std::move(connection_info));
}

} // namespace entityfy::detail
