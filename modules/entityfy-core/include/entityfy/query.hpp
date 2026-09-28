#pragma once

#include <entityfy/schema.hpp>
#include <entityfy/value.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entityfy
{

struct condition
{
    std::string            sql;
    std::vector<sql_value> binds;

    [[nodiscard]] bool empty() const noexcept
    {
        return sql.empty();
    }
};

enum class sort_direction
{
    ascending,
    descending
};

struct ordering
{
    std::string    column;
    sort_direction direction = sort_direction::ascending;
};

template <typename Model>
struct query_options
{
    condition              filter{};
    std::vector<ordering>  orderings{};
    std::optional<std::uint64_t> limit{};
    std::optional<std::uint64_t> offset{};
};

inline condition all()
{
    return {};
}

namespace detail
{

inline std::string escape_like(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for(const char character: value)
    {
        if(character == '%' || character == '_' || character == '\\')
        {
            escaped.push_back('\\');
        }

        escaped.push_back(character);
    }

    return escaped;
}

inline condition binary_condition(std::string column, std::string_view operation, sql_value value)
{
    condition output;
    output.sql = quote_identifier(column) + " " + std::string{operation} + " ?";
    output.binds.emplace_back(std::move(value));
    return output;
}

inline condition null_condition(std::string column, bool is_null)
{
    condition output;
    output.sql = quote_identifier(column) + (is_null ? " IS NULL" : " IS NOT NULL");
    return output;
}

inline condition between_condition(std::string column, sql_value lower, sql_value upper)
{
    condition output;
    output.sql = quote_identifier(column) + " BETWEEN ? AND ?";
    output.binds.emplace_back(std::move(lower));
    output.binds.emplace_back(std::move(upper));
    return output;
}

inline condition in_condition(std::string column, std::vector<sql_value> values)
{
    condition output;

    if(values.empty())
    {
        output.sql = "1 = 0";
        return output;
    }

    output.sql = quote_identifier(column) + " IN (";
    for(std::size_t index = 0; index < values.size(); ++index)
    {
        if(index != 0)
        {
            output.sql.append(", ");
        }

        output.sql.push_back('?');
    }

    output.sql.push_back(')');
    output.binds = std::move(values);
    return output;
}

} // namespace detail

inline condition operator&&(condition left, condition right)
{
    if(left.empty())
    {
        return right;
    }

    if(right.empty())
    {
        return left;
    }

    condition output;
    output.sql = "(" + std::move(left.sql) + ") AND (" + std::move(right.sql) + ")";
    output.binds.reserve(left.binds.size() + right.binds.size());
    std::ranges::move(left.binds, std::back_inserter(output.binds));
    std::ranges::move(right.binds, std::back_inserter(output.binds));
    return output;
}

inline condition operator||(condition left, condition right)
{
    if(left.empty())
    {
        return right;
    }

    if(right.empty())
    {
        return left;
    }

    condition output;
    output.sql = "(" + std::move(left.sql) + ") OR (" + std::move(right.sql) + ")";
    output.binds.reserve(left.binds.size() + right.binds.size());
    std::ranges::move(left.binds, std::back_inserter(output.binds));
    std::ranges::move(right.binds, std::back_inserter(output.binds));
    return output;
}

inline condition operator!(condition input)
{
    if(input.empty())
    {
        return input;
    }

    input.sql = "NOT (" + std::move(input.sql) + ")";
    return input;
}

template <typename Model, typename Member>
class field_predicate
{
public:
    explicit field_predicate(Member Model::* member)
        : column_{column_name(member)}
    {
    }

    template <typename Value>
    [[nodiscard]] condition eq(Value&& value) const
    {
        auto bind = detail::to_sql_value(std::forward<Value>(value));
        if(detail::is_null(bind))
        {
            return detail::null_condition(column_, true);
        }

        return detail::binary_condition(column_, "=", std::move(bind));
    }

    template <typename Value>
    [[nodiscard]] condition ne(Value&& value) const
    {
        auto bind = detail::to_sql_value(std::forward<Value>(value));
        if(detail::is_null(bind))
        {
            return detail::null_condition(column_, false);
        }

        return detail::binary_condition(column_, "<>", std::move(bind));
    }

    template <typename Value>
    [[nodiscard]] condition lt(Value&& value) const
    {
        return detail::binary_condition(column_, "<", detail::to_sql_value(std::forward<Value>(value)));
    }

    template <typename Value>
    [[nodiscard]] condition lte(Value&& value) const
    {
        return detail::binary_condition(column_, "<=", detail::to_sql_value(std::forward<Value>(value)));
    }

    template <typename Value>
    [[nodiscard]] condition gt(Value&& value) const
    {
        return detail::binary_condition(column_, ">", detail::to_sql_value(std::forward<Value>(value)));
    }

    template <typename Value>
    [[nodiscard]] condition gte(Value&& value) const
    {
        return detail::binary_condition(column_, ">=", detail::to_sql_value(std::forward<Value>(value)));
    }

    template <typename Lower, typename Upper>
    [[nodiscard]] condition between(Lower&& lower, Upper&& upper) const
    {
        return detail::between_condition(
            column_,
            detail::to_sql_value(std::forward<Lower>(lower)),
            detail::to_sql_value(std::forward<Upper>(upper))
        );
    }

    template <typename Value>
    [[nodiscard]] condition in(std::initializer_list<Value> values) const
    {
        std::vector<sql_value> binds;
        binds.reserve(values.size());

        for(const auto& value: values)
        {
            binds.emplace_back(detail::to_sql_value(value));
        }

        return detail::in_condition(column_, std::move(binds));
    }

    template <typename Range>
    [[nodiscard]] condition in_range(const Range& values) const
    {
        std::vector<sql_value> binds;

        for(const auto& value: values)
        {
            binds.emplace_back(detail::to_sql_value(value));
        }

        return detail::in_condition(column_, std::move(binds));
    }

    [[nodiscard]] condition is_null() const
    {
        return detail::null_condition(column_, true);
    }

    [[nodiscard]] condition is_not_null() const
    {
        return detail::null_condition(column_, false);
    }

    [[nodiscard]] condition starts_with(std::string_view value) const
    {
        return like(detail::escape_like(value) + "%");
    }

    [[nodiscard]] condition ends_with(std::string_view value) const
    {
        return like("%" + detail::escape_like(value));
    }

    [[nodiscard]] condition contains(std::string_view value) const
    {
        return like("%" + detail::escape_like(value) + "%");
    }

    template <typename Value>
    [[nodiscard]] condition operator==(Value&& value) const
    {
        return eq(std::forward<Value>(value));
    }

    template <typename Value>
    [[nodiscard]] condition operator!=(Value&& value) const
    {
        return ne(std::forward<Value>(value));
    }

    template <typename Value>
    [[nodiscard]] condition operator<(Value&& value) const
    {
        return lt(std::forward<Value>(value));
    }

    template <typename Value>
    [[nodiscard]] condition operator<=(Value&& value) const
    {
        return lte(std::forward<Value>(value));
    }

    template <typename Value>
    [[nodiscard]] condition operator>(Value&& value) const
    {
        return gt(std::forward<Value>(value));
    }

    template <typename Value>
    [[nodiscard]] condition operator>=(Value&& value) const
    {
        return gte(std::forward<Value>(value));
    }

private:
    [[nodiscard]] condition like(std::string pattern) const
    {
        condition output;
        output.sql = detail::quote_identifier(column_) + " LIKE ? ESCAPE '\\'";
        output.binds.emplace_back(std::move(pattern));
        return output;
    }

    std::string column_;
};

template <typename Model>
class query_builder
{
public:
    query_builder() = default;

    explicit query_builder(condition filter)
        : options_{.filter = std::move(filter)}
    {
    }

    [[nodiscard]] query_builder where(condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(filter);
        return copy;
    }

    [[nodiscard]] query_builder and_where(condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(copy.options_.filter) && std::move(filter);
        return copy;
    }

    [[nodiscard]] query_builder or_where(condition filter) const
    {
        auto copy = *this;
        copy.options_.filter = std::move(copy.options_.filter) || std::move(filter);
        return copy;
    }

    template <typename Member>
    [[nodiscard]] query_builder order_by(Member Model::* member, sort_direction direction = sort_direction::ascending) const
    {
        auto copy = *this;
        copy.options_.orderings.emplace_back(ordering{
            .column = column_name(member),
            .direction = direction,
        });
        return copy;
    }

    template <typename Member>
    [[nodiscard]] query_builder order_by_desc(Member Model::* member) const
    {
        return order_by(member, sort_direction::descending);
    }

    [[nodiscard]] query_builder take(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.limit = count;
        return copy;
    }

    [[nodiscard]] query_builder limit(std::uint64_t count) const
    {
        return take(count);
    }

    [[nodiscard]] query_builder skip(std::uint64_t count) const
    {
        auto copy = *this;
        copy.options_.offset = count;
        return copy;
    }

    [[nodiscard]] query_builder offset(std::uint64_t count) const
    {
        return skip(count);
    }

    [[nodiscard]] const query_options<Model>& options() const noexcept
    {
        return options_;
    }

    [[nodiscard]] operator query_options<Model>() const
    {
        return options_;
    }

private:
    query_options<Model> options_{};
};

template <typename Model, typename Member>
[[nodiscard]] field_predicate<Model, Member> where(Member Model::* member)
{
    return field_predicate<Model, Member>{member};
}

template <typename Model>
[[nodiscard]] query_builder<Model> query(condition filter = {})
{
    return query_builder<Model>{std::move(filter)};
}

template <typename Model, typename Member>
[[nodiscard]] ordering order_by(Member Model::* member, sort_direction direction = sort_direction::ascending)
{
    return ordering{
        .column = column_name(member),
        .direction = direction,
    };
}

template <typename Model, typename Member>
[[nodiscard]] ordering order_by_desc(Member Model::* member)
{
    return order_by(member, sort_direction::descending);
}

} // namespace entityfy
