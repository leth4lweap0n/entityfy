#pragma once

#include <entityfy/value.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace entityfy
{

namespace detail
{

// ---------------------------------------------------------------------------
// Canonical value form
//
// row_key equality, ordering and hashing compare canonical parts element-wise,
// never concatenated text: ("a", "bc") and ("ab", "c") stay different keys.
// The canonical form is a zero-copy view over the stored sql_value.
//
// Numbers compare by numeric value. int64, uint64 and integral doubles share
// one representation, so int64{1}, uint64{1} and double{1.0} are the same key
// value:
//   - the magnitude of an integral value is kept in uint64;
//   - INT64_MIN does not overflow (the magnitude is computed before negating);
//   - a uint64 above INT64_MAX keeps its real magnitude instead of wrapping
//     into a negative int64, so it stays distinct from every negative value;
//   - doubles that are not exactly integral (or are outside the uint64 range)
//     stay reals and compare with integers by value; IEEE-754 NaN compares
//     equal to NaN and sorts after every other number.
// Strings and byte blobs compare by content and length, monotonically.
// ---------------------------------------------------------------------------

struct canonical_number
{
    enum class form : unsigned char
    {
        integral = 0,
        real = 1,
        not_a_number = 2,
    };

    form          kind = form::integral;
    bool          negative = false;
    std::uint64_t magnitude = 0; // integral magnitude, |value| when kind == integral
    double        real = 0.0;    // |value| when kind == real
};

inline constexpr double canonical_uint64_limit = 18446744073709551616.0; // 2^64
inline constexpr std::uint64_t canonical_exact_double_limit = std::uint64_t{1} << 53;

[[nodiscard]] inline canonical_number canonicalize_number(std::int64_t value)
{
    if(value >= 0)
    {
        return canonical_number{
            .kind = canonical_number::form::integral,
            .negative = false,
            .magnitude = static_cast<std::uint64_t>(value),
        };
    }

    // -(value + 1) + 1 never overflows, including for INT64_MIN.
    const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
    return canonical_number{
        .kind = canonical_number::form::integral,
        .negative = true,
        .magnitude = magnitude,
    };
}

[[nodiscard]] inline canonical_number canonicalize_number(std::uint64_t value)
{
    return canonical_number{
        .kind = canonical_number::form::integral,
        .negative = false,
        .magnitude = value,
    };
}

[[nodiscard]] inline canonical_number canonicalize_number(double value)
{
    if(std::isnan(value))
    {
        return canonical_number{.kind = canonical_number::form::not_a_number};
    }

    if(value == 0.0)
    {
        // +0.0 and -0.0 are the same key value.
        return canonical_number{};
    }

    const bool   negative = value < 0.0;
    const double magnitude = negative ? -value : value;

    if(magnitude < canonical_uint64_limit)
    {
        const auto integral = static_cast<std::uint64_t>(magnitude);
        if(static_cast<double>(integral) == magnitude)
        {
            return canonical_number{
                .kind = canonical_number::form::integral,
                .negative = negative,
                .magnitude = integral,
            };
        }
    }

    return canonical_number{
        .kind = canonical_number::form::real,
        .negative = negative,
        .real = magnitude,
    };
}

// `<=>` on doubles yields std::partial_ordering, and `-ordering` does not
// exist on std::strong_ordering. Keys must expose a strong total order, so
// float comparisons go through totalize() and negations through
// reverse_ordering().
[[nodiscard]] inline std::strong_ordering totalize(std::partial_ordering ordering) noexcept
{
    if(ordering == std::partial_ordering::less)
    {
        return std::strong_ordering::less;
    }

    if(ordering == std::partial_ordering::greater)
    {
        return std::strong_ordering::greater;
    }

    if(ordering == std::partial_ordering::equivalent)
    {
        return std::strong_ordering::equal;
    }

    // Unreachable: NaN is canonicalized to form::not_a_number before any
    // numeric comparison, and compare_magnitude() keeps NaN out of the
    // double comparisons. Deterministic fallback so the order stays total.
    return std::strong_ordering::greater;
}

[[nodiscard]] inline std::strong_ordering reverse_ordering(std::strong_ordering ordering) noexcept
{
    if(ordering == std::strong_ordering::less)
    {
        return std::strong_ordering::greater;
    }

    if(ordering == std::strong_ordering::greater)
    {
        return std::strong_ordering::less;
    }

    return std::strong_ordering::equal;
}

// Compares the magnitudes of two numbers that agree in sign.
[[nodiscard]] inline std::strong_ordering compare_magnitude(const canonical_number& left, const canonical_number& right)
{
    using form = canonical_number::form;

    const bool left_nan = left.kind == form::not_a_number;
    const bool right_nan = right.kind == form::not_a_number;

    if(left_nan || right_nan)
    {
        return left_nan <=> right_nan; // NaN equals NaN and sorts last
    }

    if(left.kind == form::integral && right.kind == form::integral)
    {
        return left.magnitude <=> right.magnitude;
    }

    if(left.kind == form::real && right.kind == form::real)
    {
        return totalize(left.real <=> right.real);
    }

    // Mixed integral/real: keep the comparison exact. A real below 2^64 is
    // either not an exact integer or is an integer that would have been
    // canonicalized, so each branch keeps doubles and uint64 exact.
    const canonical_number& integral = left.kind == form::integral ? left : right;
    const canonical_number& real = left.kind == form::integral ? right : left;

    std::strong_ordering result = std::strong_ordering::less;

    if(real.real >= canonical_uint64_limit)
    {
        result = std::strong_ordering::less; // every uint64 magnitude is below 2^64
    }
    else
    {
        const auto real_integral = static_cast<std::uint64_t>(real.real);
        if(static_cast<double>(real_integral) == real.real)
        {
            result = integral.magnitude <=> real_integral;
        }
        else if(integral.magnitude > canonical_exact_double_limit)
        {
            // A non-integral double below 2^52 is smaller than 2^53.
            result = std::strong_ordering::greater;
        }
        else
        {
            result = totalize(static_cast<double>(integral.magnitude) <=> real.real);
        }
    }

    // `left` was the integral operand when it is not `right`, so the mixed
    // comparison above was computed with the integral side first; it has to be
    // reversed when the integral operand is actually `right`.
    return left.kind == form::integral ? result : reverse_ordering(result);
}

[[nodiscard]] inline std::strong_ordering compare_canonical_number(const canonical_number& left, const canonical_number& right)
{
    using form = canonical_number::form;

    const bool left_nan = left.kind == form::not_a_number;
    const bool right_nan = right.kind == form::not_a_number;

    if(left_nan || right_nan)
    {
        return left_nan <=> right_nan; // NaN equals NaN and sorts last
    }

    if(left.negative != right.negative)
    {
        return left.negative ? std::strong_ordering::less : std::strong_ordering::greater;
    }

    const auto comparison = compare_magnitude(left, right);
    return left.negative ? reverse_ordering(comparison) : comparison;
}

[[nodiscard]] inline std::strong_ordering compare_bytes(std::span<const std::byte> left, std::span<const std::byte> right)
{
    const auto common = std::min(left.size(), right.size());

    for(std::size_t index = 0; index < common; ++index)
    {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);

        if(left_byte != right_byte)
        {
            return left_byte <=> right_byte;
        }
    }

    return left.size() <=> right.size();
}

struct canonical_sql_value
{
    enum class kind : unsigned char
    {
        null = 0,
        boolean = 1,
        number = 2,
        text = 3,
        bytes = 4,
    };

    kind                       category = kind::null;
    bool                       flag = false;
    canonical_number           number{};
    std::string_view           text{};
    std::span<const std::byte> blob{};

    [[nodiscard]] static canonical_sql_value from(const sql_value& value)
    {
        canonical_sql_value output;

        std::visit(
            [&](const auto& current) {
                using current_type = std::remove_cvref_t<decltype(current)>;

                if constexpr(std::same_as<current_type, std::monostate>)
                {
                    output.category = kind::null;
                }
                else if constexpr(std::same_as<current_type, bool>)
                {
                    output.category = kind::boolean;
                    output.flag = current;
                }
                else if constexpr(std::same_as<current_type, std::string>)
                {
                    output.category = kind::text;
                    output.text = current;
                }
                else if constexpr(is_byte_vector_v<current_type>)
                {
                    output.category = kind::bytes;
                    output.blob = std::span<const std::byte>{current.data(), current.size()};
                }
                else
                {
                    output.category = kind::number;
                    output.number = canonicalize_number(current);
                }
            },
            value
        );

        return output;
    }

    friend bool operator==(const canonical_sql_value& left, const canonical_sql_value& right)
    {
        return (left <=> right) == std::strong_ordering::equal;
    }

    friend std::strong_ordering operator<=>(const canonical_sql_value& left, const canonical_sql_value& right)
    {
        if(left.category != right.category)
        {
            return left.category <=> right.category;
        }

        switch(left.category)
        {
            case kind::null:
                return std::strong_ordering::equal;
            case kind::boolean:
                return left.flag <=> right.flag;
            case kind::number:
                return compare_canonical_number(left.number, right.number);
            case kind::text:
                return left.text <=> right.text;
            case kind::bytes:
                return compare_bytes(left.blob, right.blob);
        }

        return std::strong_ordering::equal;
    }
};

[[nodiscard]] inline std::size_t hash_combine(std::size_t seed, std::size_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

// Hashes exactly what canonical_sql_value compares, so equal parts always
// produce the same hash (NaN included).
[[nodiscard]] inline std::size_t canonical_hash(const sql_value& value) noexcept
{
    const auto canonical = canonical_sql_value::from(value);

    std::size_t seed = static_cast<std::size_t>(canonical.category);

    switch(canonical.category)
    {
        case canonical_sql_value::kind::null:
            break;
        case canonical_sql_value::kind::boolean:
            seed = hash_combine(seed, std::hash<bool>{}(canonical.flag));
            break;
        case canonical_sql_value::kind::number:
        {
            const auto& number = canonical.number;
            seed = hash_combine(seed, static_cast<std::size_t>(number.kind));

            if(number.kind == canonical_number::form::not_a_number)
            {
                break; // every NaN compares equal, so it hashes to one value
            }

            seed = hash_combine(seed, std::hash<bool>{}(number.negative));

            if(number.kind == canonical_number::form::integral)
            {
                seed = hash_combine(seed, std::hash<std::uint64_t>{}(number.magnitude));
            }
            else
            {
                seed = hash_combine(seed, std::hash<double>{}(number.real));
            }

            break;
        }
        case canonical_sql_value::kind::text:
            seed = hash_combine(seed, std::hash<std::string_view>{}(canonical.text));
            break;
        case canonical_sql_value::kind::bytes:
            for(const auto item: canonical.blob)
            {
                seed = hash_combine(seed, std::hash<unsigned char>{}(static_cast<unsigned char>(item)));
            }

            break;
    }

    return seed;
}

inline void append_diagnostic_text(std::string& output, std::string_view text)
{
    output.push_back('"');

    for(const char character: text)
    {
        switch(character)
        {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                output.push_back(character);
                break;
        }
    }

    output.push_back('"');
}

inline void append_diagnostic_number(std::string& output, double value)
{
    char       buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    output.append(buffer, converted.ptr);
}

inline void append_diagnostic_bytes(std::string& output, std::span<const std::byte> blob)
{
    static constexpr char digits[] = "0123456789abcdef";

    output.append("x'");

    for(const auto item: blob)
    {
        const auto value = static_cast<unsigned char>(item);
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0F]);
    }

    output.push_back('\'');
}

inline void append_diagnostic_part(std::string& output, const sql_value& value)
{
    std::visit(
        [&](const auto& current) {
            using current_type = std::remove_cvref_t<decltype(current)>;

            if constexpr(std::same_as<current_type, std::monostate>)
            {
                output.append("NULL");
            }
            else if constexpr(std::same_as<current_type, bool>)
            {
                output.append(current ? "true" : "false");
            }
            else if constexpr(std::same_as<current_type, std::int64_t> || std::same_as<current_type, std::uint64_t>)
            {
                output.append(std::to_string(current));
            }
            else if constexpr(std::same_as<current_type, double>)
            {
                append_diagnostic_number(output, current);
            }
            else if constexpr(std::same_as<current_type, std::string>)
            {
                append_diagnostic_text(output, current);
            }
            else
            {
                append_diagnostic_bytes(output, std::span<const std::byte>{current.data(), current.size()});
            }
        },
        value
    );
}

} // namespace detail

// Ordered primary-key values. A key part is one SQL value; the key is the
// identity of a row, so equality, ordering and hashing compare canonical parts
// element-wise. The empty key is a valid, distinct value (it sorts first).
class row_key
{
public:
    row_key() = default;

    explicit row_key(std::vector<sql_value> parts)
        : parts_{std::move(parts)}
    {
    }

    explicit row_key(std::initializer_list<sql_value> parts)
        : parts_{parts}
    {
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return parts_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return parts_.size();
    }

    [[nodiscard]] const sql_value& part(std::size_t index) const
    {
        if(index >= parts_.size())
        {
            throw std::out_of_range{"entityfy ORM row_key part index is out of range"};
        }

        return parts_[index];
    }

    [[nodiscard]] std::span<const sql_value> parts() const noexcept
    {
        return parts_;
    }

    // Human-readable form for diagnostics and error messages. It is not an
    // identity: the text is never used for comparison or lookup.
    [[nodiscard]] std::string to_string() const
    {
        std::string output{"("};

        for(std::size_t index = 0; index < parts_.size(); ++index)
        {
            if(index != 0)
            {
                output.append(", ");
            }

            detail::append_diagnostic_part(output, parts_[index]);
        }

        output.push_back(')');
        return output;
    }

    friend bool operator==(const row_key& left, const row_key& right)
    {
        return (left <=> right) == std::strong_ordering::equal;
    }

    friend std::strong_ordering operator<=>(const row_key& left, const row_key& right)
    {
        const auto common = std::min(left.size(), right.size());

        for(std::size_t index = 0; index < common; ++index)
        {
            const auto comparison =
                detail::canonical_sql_value::from(left.parts_[index]) <=> detail::canonical_sql_value::from(right.parts_[index]);

            if(comparison != std::strong_ordering::equal)
            {
                return comparison;
            }
        }

        return left.size() <=> right.size();
    }

private:
    std::vector<sql_value> parts_{};
};

struct row_key_hash
{
    [[nodiscard]] std::size_t operator()(const row_key& key) const noexcept
    {
        std::size_t seed = key.size();

        for(const auto& part: key.parts())
        {
            seed = detail::hash_combine(seed, detail::canonical_hash(part));
        }

        return seed;
    }
};

} // namespace entityfy
