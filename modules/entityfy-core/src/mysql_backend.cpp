#include <entityfy/detail/backend.hpp>
#include <entityfy/error.hpp>
#include <entityfy/statement.hpp>

#include <mysql.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy::detail
{
namespace
{

struct mysql_connection_options
{
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string password;
    std::string database;
    unsigned    port = 3306;
};

// mysql://user:password@host:port/database, with the optional parts omitted.
[[nodiscard]] mysql_connection_options parse_mysql_uri(std::string_view uri)
{
    constexpr std::string_view prefix = "mysql://";
    if(!uri.starts_with(prefix))
    {
        throw std::invalid_argument{"MySQL connection string must start with mysql://"};
    }

    std::string_view        rest = uri.substr(prefix.size());
    mysql_connection_options options;

    if(const auto slash = rest.find('/'); slash != std::string_view::npos)
    {
        options.database = std::string{rest.substr(slash + 1)};
        rest = rest.substr(0, slash);
    }

    if(const auto at = rest.rfind('@'); at != std::string_view::npos)
    {
        const auto credentials = rest.substr(0, at);
        rest = rest.substr(at + 1);

        if(const auto colon = credentials.find(':'); colon != std::string_view::npos)
        {
            options.user = std::string{credentials.substr(0, colon)};
            options.password = std::string{credentials.substr(colon + 1)};
        }
        else
        {
            options.user = std::string{credentials};
        }
    }

    if(const auto colon = rest.rfind(':'); colon != std::string_view::npos)
    {
        options.host = std::string{rest.substr(0, colon)};
        options.port = static_cast<unsigned>(std::stoul(std::string{rest.substr(colon + 1)}));
    }
    else if(!rest.empty())
    {
        options.host = std::string{rest};
    }

    if(options.database.empty())
    {
        throw std::invalid_argument{"MySQL URI must include a database, for example mysql://root:secret@127.0.0.1:3306/app"};
    }

    return options;
}

struct mysql_handle_deleter
{
    void operator()(MYSQL* handle) const noexcept
    {
        if(handle != nullptr)
        {
            mysql_close(handle);
        }
    }
};

struct mysql_statement_deleter
{
    void operator()(MYSQL_STMT* statement) const noexcept
    {
        if(statement != nullptr)
        {
            mysql_stmt_close(statement);
        }
    }
};

using mysql_handle = std::unique_ptr<MYSQL, mysql_handle_deleter>;
using mysql_statement = std::unique_ptr<MYSQL_STMT, mysql_statement_deleter>;

[[nodiscard]] std::string classify_mysql_error(MYSQL* handle)
{
    const auto  code = mysql_errno(handle);
    std::string cause;

    switch(code)
    {
        case 1062:
            cause = "duplicate key: ";
            break;
        case 1451:
        case 1452:
            cause = "foreign key constraint: ";
            break;
        case 1048:
        case 1364:
            cause = "not null: ";
            break;
        case 3819:
        case 4025:
            cause = "check constraint: ";
            break;
        default:
            break;
    }

    cause.append(mysql_error(handle));
    cause.append(" (errno ");
    cause.append(std::to_string(code));
    cause.push_back(')');
    return cause;
}

// MySQL's prepared statement protocol only accepts data statements; DDL and
// session commands go through the text protocol.
[[nodiscard]] bool is_preparable_sql(std::string_view sql)
{
    const auto begin = sql.find_first_not_of(" \t\r\n");
    if(begin == std::string_view::npos)
    {
        return false;
    }

    constexpr std::string_view prefixes[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "REPLACE", "WITH", "VALUES"};
    for(const auto prefix: prefixes)
    {
        if(sql.size() - begin >= prefix.size() && sql.substr(begin, prefix.size()) == prefix)
        {
            return true;
        }
    }

    return false;
}

struct mysql_parameters
{
    std::deque<std::string>    texts;
    std::deque<std::int64_t>   integers;
    std::deque<std::uint64_t>  unsigned_integers;
    std::deque<double>         reals;
    std::deque<signed char>    flags;
    std::vector<MYSQL_BIND>    binds;
    std::vector<unsigned long> lengths;
    std::vector<my_bool>       nulls;

    [[nodiscard]] MYSQL_BIND* data() noexcept
    {
        return binds.empty() ? nullptr : binds.data();
    }
};

[[nodiscard]] mysql_parameters make_mysql_parameters(const std::vector<sql_value>& values)
{
    mysql_parameters output;
    output.binds.resize(values.size());
    output.lengths.resize(values.size());
    output.nulls.assign(values.size(), 0);

    for(std::size_t index = 0; index < values.size(); ++index)
    {
        MYSQL_BIND& bind = output.binds[index];
        bind = MYSQL_BIND{};
        bind.length = &output.lengths[index];
        bind.is_null = &output.nulls[index];

        std::visit(
            [&](const auto& current) {
                using current_type = std::remove_cvref_t<decltype(current)>;

                if constexpr(std::same_as<current_type, std::monostate>)
                {
                    bind.buffer_type = MYSQL_TYPE_NULL;
                }
                else if constexpr(std::same_as<current_type, bool>)
                {
                    output.flags.push_back(current ? 1 : 0);
                    bind.buffer_type = MYSQL_TYPE_TINY;
                    bind.buffer = &output.flags.back();
                }
                else if constexpr(std::same_as<current_type, std::int64_t>)
                {
                    output.integers.push_back(current);
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.buffer = &output.integers.back();
                }
                else if constexpr(std::same_as<current_type, std::uint64_t>)
                {
                    output.unsigned_integers.push_back(current);
                    bind.buffer_type = MYSQL_TYPE_LONGLONG;
                    bind.is_unsigned = 1;
                    bind.buffer = &output.unsigned_integers.back();
                }
                else if constexpr(std::same_as<current_type, double>)
                {
                    output.reals.push_back(current);
                    bind.buffer_type = MYSQL_TYPE_DOUBLE;
                    bind.buffer = &output.reals.back();
                }
                else if constexpr(std::same_as<current_type, std::string>)
                {
                    output.texts.push_back(current);
                    bind.buffer_type = MYSQL_TYPE_STRING;
                    bind.buffer = output.texts.back().data();
                    bind.buffer_length = static_cast<unsigned long>(output.texts.back().size());
                    output.lengths[index] = bind.buffer_length;
                }
                else
                {
                    output.texts.emplace_back(reinterpret_cast<const char*>(current.data()), current.size());
                    bind.buffer_type = MYSQL_TYPE_BLOB;
                    bind.buffer = output.texts.back().data();
                    bind.buffer_length = static_cast<unsigned long>(output.texts.back().size());
                    output.lengths[index] = bind.buffer_length;
                }
            },
            values[index]
        );
    }

    return output;
}

[[nodiscard]] sql_value parse_mysql_value(const MYSQL_FIELD& field, std::string_view text)
{
    switch(field.type)
    {
        case MYSQL_TYPE_NULL:
            return std::monostate{};

        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        {
            if((field.flags & UNSIGNED_FLAG) != 0)
            {
                std::uint64_t value = 0;
                std::from_chars(text.data(), text.data() + text.size(), value);
                return value;
            }

            std::int64_t value = 0;
            std::from_chars(text.data(), text.data() + text.size(), value);
            return value;
        }

        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        {
            double value = 0.0;
            std::from_chars(text.data(), text.data() + text.size(), value);
            return value;
        }

        default:
        {
            // Binary columns arrive with the binary charset; that is the only
            // reliable marker for a byte payload.
            if(field.charsetnr == 63)
            {
                bytes output(text.size());
                for(std::size_t index = 0; index < text.size(); ++index)
                {
                    output[index] = static_cast<std::byte>(static_cast<unsigned char>(text[index]));
                }

                return output;
            }

            return std::string{text};
        }
    }
}

[[nodiscard]] std::int64_t mysql_integer(const sql_value& value)
{
    if(const auto* number = std::get_if<std::int64_t>(&value); number != nullptr)
    {
        return *number;
    }

    if(const auto* number = std::get_if<std::uint64_t>(&value); number != nullptr)
    {
        return static_cast<std::int64_t>(*number);
    }

    throw std::runtime_error{"entityfy MySQL expected an integer result"};
}

} // namespace

class mysql_backend final : public backend
{
public:
    explicit mysql_backend(std::string connection_info)
    {
        static std::once_flag library_once;
        std::call_once(library_once, [] {
            mysql_library_init(0, nullptr, nullptr);
        });

        mysql_thread_init();

        const auto options = parse_mysql_uri(connection_info);
        handle_.reset(mysql_init(nullptr));

        if(!handle_)
        {
            throw std::runtime_error{"entityfy MySQL client initialization failed"};
        }

        mysql_options(handle_.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if(mysql_real_connect(
               handle_.get(),
               options.host.c_str(),
               options.user.c_str(),
               options.password.c_str(),
               options.database.c_str(),
               options.port,
               nullptr,
               0
           ) == nullptr)
        {
            throw std::runtime_error{"entityfy MySQL connection failed: " + classify_mysql_error(handle_.get())};
        }

        // Double quotes have to mean identifiers, as they do in the other
        // dialects, and silent truncation stays off.
        execute(statement{
            .sql = "SET SESSION sql_mode = "
                   "CONCAT_WS(',', @@SESSION.sql_mode, 'ANSI_QUOTES', 'STRICT_ALL_TABLES', 'NO_BACKSLASH_ESCAPES')"
        });
    }

    [[nodiscard]] dialect target_dialect() const noexcept override
    {
        return dialect::mysql;
    }

    execution_result execute(const entityfy::statement& input) override
    {
        const std::lock_guard lock{mutex_};
        mysql_thread_init();

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            if(!is_preparable_sql(input.sql))
            {
                if(!input.binds.empty())
                {
                    throw std::runtime_error{"entityfy MySQL cannot bind parameters to a non-data statement"};
                }

                if(mysql_real_query(handle_.get(), input.sql.data(), static_cast<unsigned long>(input.sql.size())) != 0)
                {
                    throw std::runtime_error{"entityfy MySQL execute failed: " + classify_mysql_error(handle_.get())};
                }

                return execution_result{
                    .rows_affected = static_cast<std::uint64_t>(mysql_affected_rows(handle_.get())),
                    .last_insert_id = static_cast<std::int64_t>(mysql_insert_id(handle_.get())),
                };
            }

            mysql_statement statement{prepare(input)};
            auto            parameters = make_mysql_parameters(input.binds);

            if(mysql_stmt_bind_param(statement.get(), parameters.data()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL bind parameters failed: " + classify_mysql_error(handle_.get())};
            }

            if(mysql_stmt_execute(statement.get()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL execute failed: " + classify_mysql_error(handle_.get())};
            }

            return execution_result{
                .rows_affected = static_cast<std::uint64_t>(mysql_stmt_affected_rows(statement.get())),
                .last_insert_id = static_cast<std::int64_t>(mysql_stmt_insert_id(statement.get())),
            };
        }
        catch(const sql_error&)
        {
            throw;
        }
        catch(const std::exception& error)
        {
            throw_classified_sql_error("MySQL", "execute", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<row> query(const entityfy::statement& input) override
    {
        std::vector<row> rows;

        query_each(input, [&](const row& current) {
            rows.push_back(current);
            return true;
        });

        return rows;
    }

    void query_each(const entityfy::statement& input,
                    const std::function<bool(const row&)>& consumer) override
    {
        const std::lock_guard lock{mutex_};
        mysql_thread_init();
        bool in_consumer = false;

        try
        {
            if(logger_)
            {
                logger_(input.sql);
            }

            mysql_statement statement{prepare(input)};
            auto            parameters = make_mysql_parameters(input.binds);

            if(mysql_stmt_bind_param(statement.get(), parameters.data()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL bind parameters failed: " + classify_mysql_error(handle_.get())};
            }

            if(mysql_stmt_execute(statement.get()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL execute failed: " + classify_mysql_error(handle_.get())};
            }

            MYSQL_RES* raw_metadata = mysql_stmt_result_metadata(statement.get());
            if(raw_metadata == nullptr)
            {
                throw std::runtime_error{"entityfy MySQL query returned no result set"};
            }

            std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata{raw_metadata, mysql_free_result};
            const auto* fields = mysql_fetch_fields(metadata.get());
            const auto  column_count = static_cast<unsigned int>(mysql_num_fields(metadata.get()));

            std::vector<MYSQL_BIND>    binds(column_count);
            std::vector<unsigned long> lengths(column_count, 0);
            std::vector<my_bool>       nulls(column_count, 0);
            std::vector<my_bool>       errors(column_count, 0);

            for(unsigned int index = 0; index < column_count; ++index)
            {
                MYSQL_BIND& bind = binds[index];
                bind = MYSQL_BIND{};
                bind.buffer_type = MYSQL_TYPE_STRING;
                bind.buffer = nullptr;
                bind.buffer_length = 0;
                bind.length = &lengths[index];
                bind.is_null = &nulls[index];
                bind.error = &errors[index];
            }

            if(mysql_stmt_bind_result(statement.get(), binds.data()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL bind result failed: " + classify_mysql_error(handle_.get())};
            }

            if(mysql_stmt_store_result(statement.get()) != 0)
            {
                throw std::runtime_error{"entityfy MySQL store result failed: " + classify_mysql_error(handle_.get())};
            }

            row current;

            while(true)
            {
                const int status = mysql_stmt_fetch(statement.get());
                if(status == MYSQL_NO_DATA)
                {
                    break;
                }

                if(status != 0 && status != MYSQL_DATA_TRUNCATED)
                {
                    throw std::runtime_error{"entityfy MySQL fetch failed: " + classify_mysql_error(handle_.get())};
                }

                current.clear();
                current.reserve(column_count);

                for(unsigned int index = 0; index < column_count; ++index)
                {
                    if(nulls[index] != 0)
                    {
                        current.emplace_back(std::monostate{});
                        continue;
                    }

                    std::string text(lengths[index], '\0');

                    MYSQL_BIND column_bind{};
                    column_bind.buffer_type = MYSQL_TYPE_STRING;
                    column_bind.buffer = text.data();
                    column_bind.buffer_length = static_cast<unsigned long>(text.size());

                    unsigned long column_length = 0;
                    my_bool       column_null = 0;
                    column_bind.length = &column_length;
                    column_bind.is_null = &column_null;

                    if(mysql_stmt_fetch_column(statement.get(), &column_bind, index, 0) != 0)
                    {
                        throw std::runtime_error{
                            "entityfy MySQL column fetch failed: " + classify_mysql_error(handle_.get())
                        };
                    }

                    if(column_null != 0)
                    {
                        current.emplace_back(std::monostate{});
                        continue;
                    }

                    text.resize(column_length);
                    current.emplace_back(parse_mysql_value(fields[index], text));
                }

                in_consumer = true;
                const bool keep = consumer(current);
                in_consumer = false;

                if(!keep)
                {
                    break;
                }
            }
        }
        catch(const sql_error&)
        {
            throw;
        }
        catch(const std::exception& error)
        {
            if(in_consumer)
            {
                throw;
            }

            throw_classified_sql_error("MySQL", "query", input.sql, error.what());
        }
    }

    [[nodiscard]] std::vector<std::string> table_columns(std::string_view table) override
    {
        statement input;
        input.sql =
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_schema = DATABASE() AND table_name = ? "
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
            "SELECT column_name, column_type, is_nullable, column_default, column_key "
            "FROM information_schema.columns "
            "WHERE table_schema = DATABASE() AND table_name = ? "
            "ORDER BY ordinal_position";
        columns_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(columns_statement))
        {
            if(result_row.size() < 5)
            {
                continue;
            }

            output.columns.emplace_back(column_info{
                .name = value_to_string(result_row[0]),
                .sql_type = value_to_string(result_row[1]),
                .nullable = value_to_string(result_row[2]) == "YES",
                .primary_key = value_to_string(result_row[4]) == "PRI",
                .default_sql = value_to_string(result_row[3]),
            });
        }

        statement indexes_statement;
        indexes_statement.sql =
            "SELECT index_name, non_unique, column_name "
            "FROM information_schema.statistics "
            "WHERE table_schema = DATABASE() AND table_name = ? "
            "ORDER BY index_name, seq_in_index";
        indexes_statement.binds.emplace_back(std::string{table});

        for(const auto& result_row: query(indexes_statement))
        {
            if(result_row.size() < 3)
            {
                continue;
            }

            const auto name = value_to_string(result_row[0]);
            const bool unique = mysql_integer(result_row[1]) == 0;
            const auto column = value_to_string(result_row[2]);

            const auto existing = std::find_if(output.indexes.begin(), output.indexes.end(), [&](const index_info& index) {
                return index.name == name;
            });

            if(existing == output.indexes.end())
            {
                output.indexes.emplace_back(index_info{.name = name, .unique = unique, .columns = {column}});
            }
            else
            {
                existing->columns.emplace_back(column);
            }
        }

        statement foreign_keys_statement;
        foreign_keys_statement.sql =
            "SELECT k.column_name, k.referenced_table_name, k.referenced_column_name, "
            "r.update_rule, r.delete_rule "
            "FROM information_schema.key_column_usage k "
            "JOIN information_schema.referential_constraints r "
            "ON r.constraint_name = k.constraint_name AND r.constraint_schema = k.table_schema "
            "WHERE k.table_schema = DATABASE() AND k.table_name = ? AND k.referenced_table_name IS NOT NULL "
            "ORDER BY k.constraint_name, k.ordinal_position";
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

    void set_batch_parameter_limit(std::size_t limit) override
    {
        std::lock_guard lock{mutex_};
        batch_parameter_limit_ = limit == 0 ? 1 : limit;
    }

    [[nodiscard]] std::size_t batch_parameter_limit() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return batch_parameter_limit_;
    }

    void set_logger(std::function<void(std::string_view)> logger) override
    {
        std::lock_guard lock{mutex_};
        logger_ = std::move(logger);
    }

private:
    [[nodiscard]] MYSQL_STMT* prepare(const statement& input)
    {
        MYSQL_STMT* created = mysql_stmt_init(handle_.get());
        if(created == nullptr)
        {
            throw std::runtime_error{"entityfy MySQL statement initialization failed: " + classify_mysql_error(handle_.get())};
        }

        mysql_statement statement{created};

        if(mysql_stmt_prepare(statement.get(), input.sql.data(), static_cast<unsigned long>(input.sql.size())) != 0)
        {
            throw std::runtime_error{"entityfy MySQL prepare failed: " + classify_mysql_error(handle_.get())};
        }

        return statement.release();
    }

    mysql_handle                          handle_{};
    mutable std::mutex                    mutex_;
    std::size_t                           batch_parameter_limit_ = 60000;
    std::function<void(std::string_view)> logger_;
};

std::unique_ptr<backend> make_mysql_backend(std::string connection_info)
{
    return std::make_unique<mysql_backend>(std::move(connection_info));
}

} // namespace entityfy::detail
