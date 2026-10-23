#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace entityfy
{

class orm_error : public std::runtime_error
{
public:
    explicit orm_error(std::string message)
        : std::runtime_error{std::move(message)}
    {
    }
};

class sql_error : public orm_error
{
public:
    sql_error(std::string message, std::string sql)
        : orm_error{std::move(message)},
          sql_{std::move(sql)}
    {
    }

    [[nodiscard]] const std::string& sql() const noexcept
    {
        return sql_;
    }

private:
    std::string sql_;
};

class query_error : public sql_error
{
public:
    using sql_error::sql_error;
};

class connection_error : public orm_error
{
public:
    using orm_error::orm_error;
};

class transaction_error : public orm_error
{
public:
    using orm_error::orm_error;
};

class migration_error : public orm_error
{
public:
    using orm_error::orm_error;
};

class constraint_error : public sql_error
{
public:
    using sql_error::sql_error;
};

class unique_violation : public constraint_error
{
public:
    using constraint_error::constraint_error;
};

class foreign_key_violation : public constraint_error
{
public:
    using constraint_error::constraint_error;
};

class not_null_violation : public constraint_error
{
public:
    using constraint_error::constraint_error;
};

class check_violation : public constraint_error
{
public:
    using constraint_error::constraint_error;
};

// Errors that a retry can resolve: the transaction lost a race, not the data.
class transient_error : public sql_error
{
public:
    using sql_error::sql_error;
};

// PostgreSQL SQLSTATE 40001: the transaction must be retried.
class serialization_failure : public transient_error
{
public:
    using transient_error::transient_error;
};

// PostgreSQL SQLSTATE 40P01: the transaction was chosen as the deadlock victim.
class deadlock_detected : public transient_error
{
public:
    using transient_error::transient_error;
};

// SQLite SQLITE_BUSY / SQLITE_LOCKED: another connection holds the lock.
class database_busy : public transient_error
{
public:
    using transient_error::transient_error;
};

namespace detail
{

inline bool contains_case_insensitive(std::string_view input, std::string_view needle)
{
    if(needle.empty())
    {
        return true;
    }

    if(input.size() < needle.size())
    {
        return false;
    }

    for(std::size_t offset = 0; offset <= input.size() - needle.size(); ++offset)
    {
        bool matches = true;
        for(std::size_t index = 0; index < needle.size(); ++index)
        {
            auto left = input[offset + index];
            auto right = needle[index];

            if(left >= 'A' && left <= 'Z')
            {
                left = static_cast<char>(left - 'A' + 'a');
            }

            if(right >= 'A' && right <= 'Z')
            {
                right = static_cast<char>(right - 'A' + 'a');
            }

            if(left != right)
            {
                matches = false;
                break;
            }
        }

        if(matches)
        {
            return true;
        }
    }

    return false;
}

inline std::string sql_context_message(std::string_view backend, std::string_view operation, std::string_view sql, std::string_view cause)
{
    std::string output{"entityfy "};
    output.append(backend);
    output.push_back(' ');
    output.append(operation);
    output.append(" failed for `");
    output.append(sql);
    output.append("`: ");
    output.append(cause);
    return output;
}

[[noreturn]] inline void throw_classified_sql_error(
    std::string_view backend,
    std::string_view operation,
    const std::string& sql,
    std::string_view cause
)
{
    auto message = sql_context_message(backend, operation, sql, cause);

    if(contains_case_insensitive(cause, "unique") || contains_case_insensitive(cause, "duplicate key"))
    {
        throw unique_violation{std::move(message), sql};
    }

    if(contains_case_insensitive(cause, "foreign key") || contains_case_insensitive(cause, "violates foreign key"))
    {
        throw foreign_key_violation{std::move(message), sql};
    }

    if(contains_case_insensitive(cause, "not null") || contains_case_insensitive(cause, "null value"))
    {
        throw not_null_violation{std::move(message), sql};
    }

    if(contains_case_insensitive(cause, "check constraint") || contains_case_insensitive(cause, "check failed"))
    {
        throw check_violation{std::move(message), sql};
    }

    throw query_error{std::move(message), sql};
}

} // namespace detail

} // namespace entityfy
