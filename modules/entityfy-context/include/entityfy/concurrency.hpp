#pragma once

#include <entityfy/error.hpp>
#include <entityfy/value.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace entityfy
{

// One failed optimistic concurrency check. `database_values` is the row as it
// exists now (empty when the row is gone); `original_token` is the value the
// failed statement matched against. `token_column` is empty when the model has
// no concurrency token.
struct concurrency_entry
{
    std::string              table;
    std::int64_t             key = 0;
    std::string              token_column;
    sql_value                original_token{};
    std::vector<std::string> columns;
    std::vector<sql_value>   database_values;

    [[nodiscard]] bool row_exists() const noexcept
    {
        return !database_values.empty();
    }
};

// Thrown when an UPDATE or DELETE affected zero rows. EF Core's
// DbUpdateConcurrencyException counterpart: the conflicting entries carry the
// key, the token and the current database row so a caller can resolve it.
class concurrency_error : public orm_error
{
public:
    concurrency_error(std::string message, std::vector<concurrency_entry> entries)
        : orm_error{std::move(message)}, entries_{std::move(entries)}
    {
    }

    [[nodiscard]] const std::vector<concurrency_entry>& entries() const noexcept
    {
        return entries_;
    }

private:
    std::vector<concurrency_entry> entries_;
};

} // namespace entityfy
