#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace entityfy
{

using bytes = std::vector<std::byte>;

using sql_value = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, bytes>;

inline sql_value to_sql_value(const sql_value& value)
{
    return value;
}

// ---------------------------------------------------------------------------
// Value converter support
//
// For user-defined types, when entityfy::value_converter<T> provides
//   static T        to_value(const sql_value&);
//   static sql_value from_value(const T&);
// the field is written to and read from a single column. Example: storing a
// composite value as JSON or text, converting a string-based field to int.
// ---------------------------------------------------------------------------
template <typename Type>
struct value_converter;

template <typename Type>
concept has_value_converter = requires(const Type& value, const sql_value& raw) {
    { value_converter<Type>::to_value(raw) } -> std::convertible_to<Type>;
    { value_converter<Type>::from_value(value) } -> std::convertible_to<sql_value>;
};

namespace detail
{

template <typename Type>
inline constexpr bool dependent_false = false;

template <typename Type>
struct is_optional : std::false_type
{
};

template <typename Type>
struct is_optional<std::optional<Type>> : std::true_type
{
    using value_type = Type;
};

template <typename Type>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<Type>>::value;

template <typename Type>
using optional_value_t = typename is_optional<std::remove_cvref_t<Type>>::value_type;

template <typename Type>
struct is_byte_vector : std::false_type
{
};

template <typename Allocator>
struct is_byte_vector<std::vector<std::byte, Allocator>> : std::true_type
{
};

template <typename Type>
inline constexpr bool is_byte_vector_v = is_byte_vector<std::remove_cvref_t<Type>>::value;

template <typename Type>
struct is_chrono_duration : std::false_type
{
};

template <typename Rep, typename Period>
struct is_chrono_duration<std::chrono::duration<Rep, Period>> : std::true_type
{
};

template <typename Type>
inline constexpr bool is_chrono_duration_v = is_chrono_duration<std::remove_cvref_t<Type>>::value;

template <typename Type>
struct is_system_time_point : std::false_type
{
};

template <typename Duration>
struct is_system_time_point<std::chrono::time_point<std::chrono::system_clock, Duration>> : std::true_type
{
};

template <typename Type>
inline constexpr bool is_system_time_point_v = is_system_time_point<std::remove_cvref_t<Type>>::value;

template <typename Type>
inline constexpr bool is_sys_days_v = std::same_as<std::remove_cvref_t<Type>, std::chrono::sys_days>;

template <typename Type>
inline constexpr bool is_native_scalar_v =
    std::same_as<std::remove_cvref_t<Type>, bool> ||
    std::integral<std::remove_cvref_t<Type>> ||
    std::floating_point<std::remove_cvref_t<Type>> ||
    std::is_enum_v<std::remove_cvref_t<Type>> ||
    std::same_as<std::remove_cvref_t<Type>, std::string> ||
    is_byte_vector_v<Type> ||
    is_chrono_duration_v<Type> ||
    is_system_time_point_v<Type>;

template <typename Type>
inline constexpr bool is_supported_scalar_v = is_native_scalar_v<Type> || has_value_converter<Type>;

template <typename Type>
struct is_supported_field : std::bool_constant<is_supported_scalar_v<std::remove_cvref_t<Type>>>
{
};

template <typename Type>
struct is_supported_field<std::optional<Type>> : std::bool_constant<is_supported_scalar_v<Type>>
{
};

template <typename Type>
inline constexpr bool is_supported_field_v = is_supported_field<std::remove_cvref_t<Type>>::value;

template <typename Type>
constexpr sql_value to_sql_value(const Type& value);

inline sql_value to_sql_value(const sql_value& value)
{
    return value;
}

inline void append_padded_number(std::string& output, int value, int width)
{
    const auto text = std::to_string(value);
    for(int index = static_cast<int>(text.size()); index < width; ++index)
    {
        output.push_back('0');
    }

    output.append(text);
}

inline std::string format_date(std::chrono::sys_days value)
{
    const std::chrono::year_month_day ymd{value};

    if(!ymd.ok())
    {
        throw std::runtime_error{"entityfy ORM cannot bind an invalid date"};
    }

    std::string output;
    output.reserve(10);
    append_padded_number(output, static_cast<int>(ymd.year()), 4);
    output.push_back('-');
    append_padded_number(output, static_cast<int>(static_cast<unsigned>(ymd.month())), 2);
    output.push_back('-');
    append_padded_number(output, static_cast<int>(static_cast<unsigned>(ymd.day())), 2);
    return output;
}

template <typename Duration>
std::string format_time(Duration value)
{
    using namespace std::chrono;

    const auto seconds_value = duration_cast<seconds>(value);
    if(seconds_value < seconds::zero() || seconds_value >= hours{24})
    {
        throw std::runtime_error{"entityfy ORM time values must be within a single day"};
    }

    const auto hours_value = duration_cast<hours>(seconds_value);
    const auto minutes_value = duration_cast<minutes>(seconds_value - hours_value);
    const auto seconds_part = seconds_value - hours_value - minutes_value;

    std::string output;
    output.reserve(8);
    append_padded_number(output, static_cast<int>(hours_value.count()), 2);
    output.push_back(':');
    append_padded_number(output, static_cast<int>(minutes_value.count()), 2);
    output.push_back(':');
    append_padded_number(output, static_cast<int>(seconds_part.count()), 2);
    return output;
}

template <typename Duration>
std::string format_timestamp(std::chrono::time_point<std::chrono::system_clock, Duration> value)
{
    using namespace std::chrono;

    const auto seconds_value = floor<seconds>(value);
    const auto days_value = floor<days>(seconds_value);
    return format_date(days_value) + " " + format_time(seconds_value - days_value);
}

inline int parse_fixed_int(std::string_view input, std::size_t offset, std::size_t count)
{
    if(offset + count > input.size())
    {
        throw std::runtime_error{"entityfy ORM date/time value is too short"};
    }

    int output = 0;
    for(std::size_t index = offset; index < offset + count; ++index)
    {
        const char current = input[index];
        if(current < '0' || current > '9')
        {
            throw std::runtime_error{"entityfy ORM date/time value contains a non-digit component"};
        }

        output = output * 10 + (current - '0');
    }

    return output;
}

inline std::chrono::sys_days parse_date(std::string_view input)
{
    if(input.size() < 10 || input[4] != '-' || input[7] != '-')
    {
        throw std::runtime_error{"entityfy ORM expected date format YYYY-MM-DD"};
    }

    const auto year = std::chrono::year{parse_fixed_int(input, 0, 4)};
    const auto month = std::chrono::month{static_cast<unsigned>(parse_fixed_int(input, 5, 2))};
    const auto day = std::chrono::day{static_cast<unsigned>(parse_fixed_int(input, 8, 2))};
    const std::chrono::year_month_day ymd{year, month, day};

    if(!ymd.ok())
    {
        throw std::runtime_error{"entityfy ORM received an invalid calendar date"};
    }

    return std::chrono::sys_days{ymd};
}

inline std::chrono::seconds parse_time(std::string_view input)
{
    if(input.size() < 8 || input[2] != ':' || input[5] != ':')
    {
        throw std::runtime_error{"entityfy ORM expected time format HH:MM:SS"};
    }

    const auto hour = parse_fixed_int(input, 0, 2);
    const auto minute = parse_fixed_int(input, 3, 2);
    const auto second = parse_fixed_int(input, 6, 2);

    if(hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        throw std::runtime_error{"entityfy ORM received an invalid time value"};
    }

    return std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
}

inline std::chrono::sys_seconds parse_timestamp(std::string_view input)
{
    if(input.size() < 19 || (input[10] != ' ' && input[10] != 'T'))
    {
        throw std::runtime_error{"entityfy ORM expected timestamp format YYYY-MM-DD HH:MM:SS"};
    }

    return std::chrono::sys_seconds{parse_date(input.substr(0, 10)) + parse_time(input.substr(11, 8))};
}

template <typename Type>
constexpr sql_value scalar_to_sql_value(const Type& value)
{
    using converter_type = std::remove_cvref_t<Type>;

    using value_type = std::remove_cvref_t<Type>;

    if constexpr(has_value_converter<value_type>)
    {
        return value_converter<value_type>::from_value(value);
    }
    else if constexpr(std::same_as<value_type, bool>)
    {
        return value;
    }
    else if constexpr(std::is_enum_v<value_type>)
    {
        using underlying = std::underlying_type_t<value_type>;
        return scalar_to_sql_value(static_cast<underlying>(value));
    }
    else if constexpr(std::signed_integral<value_type>)
    {
        return static_cast<std::int64_t>(value);
    }
    else if constexpr(std::unsigned_integral<value_type>)
    {
        return static_cast<std::uint64_t>(value);
    }
    else if constexpr(std::floating_point<value_type>)
    {
        return static_cast<double>(value);
    }
    else if constexpr(std::same_as<value_type, std::string>)
    {
        return value;
    }
    else if constexpr(is_sys_days_v<value_type>)
    {
        return format_date(value);
    }
    else if constexpr(is_system_time_point_v<value_type>)
    {
        return format_timestamp(value);
    }
    else if constexpr(is_chrono_duration_v<value_type>)
    {
        return format_time(value);
    }
    else if constexpr(is_byte_vector_v<value_type>)
    {
        return bytes{value.begin(), value.end()};
    }
    else
    {
        static_assert(dependent_false<value_type>, "entityfy ORM cannot bind this value type. Use bool, integral, floating point, enum, std::string, std::vector<std::byte>, or std::optional<T>.");
    }
}

template <typename Type>
constexpr sql_value to_sql_value(const Type& value)
{
    using value_type = std::remove_cvref_t<Type>;

    if constexpr(is_optional_v<value_type>)
    {
        if(!value)
        {
            return std::monostate{};
        }

        return scalar_to_sql_value(*value);
    }
    else
    {
        return scalar_to_sql_value(value);
    }
}

inline sql_value to_sql_value(const char* value)
{
    return value == nullptr ? sql_value{std::monostate{}} : sql_value{std::string{value}};
}

template <std::size_t Size>
inline sql_value to_sql_value(const char (&value)[Size])
{
    return sql_value{std::string{value}};
}

inline bool is_null(const sql_value& value)
{
    return std::holds_alternative<std::monostate>(value);
}

inline std::string value_to_string(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> std::string {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                return {};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return current ? "1" : "0";
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return current;
            }
            else if constexpr(std::same_as<current_type, bytes>)
            {
                return {};
            }
            else
            {
                return std::to_string(current);
            }
        },
        value
    );
}

inline std::int64_t value_to_i64(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> std::int64_t {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                throw std::runtime_error{"entityfy ORM cannot read NULL into a non-optional integral field"};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return current ? 1 : 0;
            }
            else if constexpr(std::same_as<current_type, std::int64_t>)
            {
                return current;
            }
            else if constexpr(std::same_as<current_type, std::uint64_t>)
            {
                if(current > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                {
                    throw std::overflow_error{"entityfy ORM integer value does not fit in std::int64_t"};
                }

                return static_cast<std::int64_t>(current);
            }
            else if constexpr(std::same_as<current_type, double>)
            {
                return static_cast<std::int64_t>(current);
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return std::stoll(current);
            }
            else
            {
                throw std::runtime_error{"entityfy ORM cannot read BLOB into an integral field"};
            }
        },
        value
    );
}

inline std::uint64_t value_to_u64(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> std::uint64_t {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                throw std::runtime_error{"entityfy ORM cannot read NULL into a non-optional integral field"};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return current ? 1U : 0U;
            }
            else if constexpr(std::same_as<current_type, std::int64_t>)
            {
                if(current < 0)
                {
                    throw std::overflow_error{"entityfy ORM negative integer does not fit in an unsigned field"};
                }

                return static_cast<std::uint64_t>(current);
            }
            else if constexpr(std::same_as<current_type, std::uint64_t>)
            {
                return current;
            }
            else if constexpr(std::same_as<current_type, double>)
            {
                return static_cast<std::uint64_t>(current);
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return std::stoull(current);
            }
            else
            {
                throw std::runtime_error{"entityfy ORM cannot read BLOB into an integral field"};
            }
        },
        value
    );
}

inline double value_to_double(const sql_value& value)
{
    return std::visit(
        [](const auto& current) -> double {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                throw std::runtime_error{"entityfy ORM cannot read NULL into a non-optional floating point field"};
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                return current ? 1.0 : 0.0;
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                return std::stod(current);
            }
            else if constexpr(std::same_as<current_type, bytes>)
            {
                throw std::runtime_error{"entityfy ORM cannot read BLOB into a floating point field"};
            }
            else
            {
                return static_cast<double>(current);
            }
        },
        value
    );
}

inline bool value_to_bool(const sql_value& value)
{
    if(std::holds_alternative<std::string>(value))
    {
        const auto& text = std::get<std::string>(value);
        return text == "1" || text == "t" || text == "true" || text == "TRUE";
    }

    return value_to_i64(value) != 0;
}

template <typename Output>
[[nodiscard]] Output checked_signed_integral_cast(std::int64_t value)
{
    static_assert(std::signed_integral<Output>);

    if(value < static_cast<std::int64_t>(std::numeric_limits<Output>::min()) ||
       value > static_cast<std::int64_t>(std::numeric_limits<Output>::max()))
    {
        throw std::overflow_error{"entityfy ORM integer value does not fit in the destination signed field"};
    }

    return static_cast<Output>(value);
}

template <typename Output>
[[nodiscard]] Output checked_unsigned_integral_cast(std::uint64_t value)
{
    static_assert(std::unsigned_integral<Output>);

    if(value > static_cast<std::uint64_t>(std::numeric_limits<Output>::max()))
    {
        throw std::overflow_error{"entityfy ORM integer value does not fit in the destination unsigned field"};
    }

    return static_cast<Output>(value);
}

template <typename Type>
void assign_sql_value(const sql_value& value, Type& output)
{
    using output_type = std::remove_cvref_t<Type>;

    if constexpr(has_value_converter<output_type>)
    {
        output = value_converter<output_type>::to_value(value);
    }
    else if constexpr(is_optional_v<output_type>)
    {
        if(is_null(value))
        {
            output = std::nullopt;
        }
        else
        {
            optional_value_t<output_type> inner{};
            assign_sql_value(value, inner);
            output = std::move(inner);
        }
    }
    else if constexpr(std::same_as<output_type, bool>)
    {
        output = value_to_bool(value);
    }
    else if constexpr(std::is_enum_v<output_type>)
    {
        using underlying = std::underlying_type_t<output_type>;
        underlying inner{};
        assign_sql_value(value, inner);
        output = static_cast<output_type>(inner);
    }
    else if constexpr(std::signed_integral<output_type>)
    {
        output = checked_signed_integral_cast<output_type>(value_to_i64(value));
    }
    else if constexpr(std::unsigned_integral<output_type>)
    {
        output = checked_unsigned_integral_cast<output_type>(value_to_u64(value));
    }
    else if constexpr(std::floating_point<output_type>)
    {
        output = static_cast<output_type>(value_to_double(value));
    }
    else if constexpr(std::same_as<output_type, std::string>)
    {
        if(is_null(value))
        {
            throw std::runtime_error{"entityfy ORM cannot read NULL into std::string; use std::optional<std::string>"};
        }

        output = value_to_string(value);
    }
    else if constexpr(is_sys_days_v<output_type>)
    {
        if(is_null(value))
        {
            throw std::runtime_error{"entityfy ORM cannot read NULL into std::chrono::sys_days; use std::optional<std::chrono::sys_days>"};
        }

        output = parse_date(value_to_string(value));
    }
    else if constexpr(is_system_time_point_v<output_type>)
    {
        if(is_null(value))
        {
            throw std::runtime_error{"entityfy ORM cannot read NULL into std::chrono::system_clock::time_point; use std::optional<T>"};
        }

        using duration_type = typename output_type::duration;
        output = std::chrono::time_point_cast<duration_type>(parse_timestamp(value_to_string(value)));
    }
    else if constexpr(is_chrono_duration_v<output_type>)
    {
        if(is_null(value))
        {
            throw std::runtime_error{"entityfy ORM cannot read NULL into a chrono duration; use std::optional<T>"};
        }

        output = std::chrono::duration_cast<output_type>(parse_time(value_to_string(value)));
    }
    else if constexpr(is_byte_vector_v<output_type>)
    {
        if(!std::holds_alternative<bytes>(value))
        {
            throw std::runtime_error{"entityfy ORM cannot read a non-BLOB value into std::vector<std::byte>"};
        }

        output = std::get<bytes>(value);
    }
    else
    {
        static_assert(dependent_false<output_type>, "entityfy ORM cannot materialize this field type.");
    }
}

} // namespace detail

} // namespace entityfy
