#include <catch2/catch_test_macros.hpp>

#include <entityfy/key.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using entityfy::row_key;
using entityfy::sql_value;

constexpr std::int64_t int64_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t int64_max = std::numeric_limits<std::int64_t>::max();

[[nodiscard]] row_key one_part(sql_value value)
{
    return row_key{std::vector<sql_value>{std::move(value)}};
}

[[nodiscard]] row_key make_key(std::initializer_list<sql_value> parts)
{
    return row_key{parts};
}

[[nodiscard]] row_key int_key(std::int64_t value)
{
    return one_part(sql_value{value});
}

[[nodiscard]] row_key uint_key(std::uint64_t value)
{
    return one_part(sql_value{value});
}

[[nodiscard]] row_key real_key(double value)
{
    return one_part(sql_value{value});
}

[[nodiscard]] row_key text_key(std::string value)
{
    return one_part(sql_value{std::move(value)});
}

[[nodiscard]] row_key bytes_key(entityfy::bytes value)
{
    return one_part(sql_value{std::move(value)});
}

[[nodiscard]] std::size_t hash_of(const row_key& key)
{
    return entityfy::row_key_hash{}(key);
}

const double quiet_nan = std::numeric_limits<double>::quiet_NaN();

} // namespace

TEST_CASE("row_key default construction is the empty key", "[key]")
{
    const row_key empty;

    REQUIRE(empty.empty());
    REQUIRE(empty.size() == 0);
    REQUIRE(empty.parts().empty());
    REQUIRE(empty.to_string() == "()");

    // The empty key is a distinct value that sorts before every non-empty key.
    REQUIRE(empty != int_key(0));
    REQUIRE(empty < int_key(0));
    REQUIRE(empty < make_key({sql_value{}}));

    REQUIRE_THROWS_AS(static_cast<void>(empty.part(0)), std::out_of_range);
}

TEST_CASE("row_key constructs from a vector and from an initializer list", "[key]")
{
    const std::vector<sql_value> parts{sql_value{std::int64_t{7}}, sql_value{std::string{"eu"}}};

    const row_key from_vector{parts};
    const row_key from_list{sql_value{std::int64_t{7}}, sql_value{std::string{"eu"}}};

    REQUIRE(from_vector.size() == 2);
    REQUIRE(from_vector.parts().size() == 2);
    REQUIRE(from_list.size() == 2);
    REQUIRE(from_vector == from_list);
    REQUIRE(from_vector.part(0) == parts[0]);

    // Both constructors are explicit on purpose: a bare vector is not a key.
    static_assert(!std::is_convertible_v<std::vector<sql_value>, row_key>);
    static_assert(!std::is_convertible_v<std::initializer_list<sql_value>, row_key>);
    static_assert(std::is_constructible_v<row_key, std::vector<sql_value>>);
    static_assert(std::is_constructible_v<row_key, std::initializer_list<sql_value>>);
}

TEST_CASE("row_key equality compares parts element-wise", "[key]")
{
    // Concatenating the parts would make these two keys collide; the key
    // compares elements, so they stay different values.
    const row_key split_left = make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}});
    const row_key split_right = make_key({sql_value{std::string{"ab"}}, sql_value{std::string{"c"}}});

    REQUIRE(split_left != split_right);
    REQUIRE(hash_of(split_left) != hash_of(split_right));

    // Identical parts are equal regardless of how the key was built.
    REQUIRE(split_left == make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}}));
    REQUIRE(make_key({sql_value{std::int64_t{1}}, sql_value{std::string{"a"}}}) ==
            row_key{std::vector<sql_value>{sql_value{std::int64_t{1}}, sql_value{std::string{"a"}}}});

    // A different part count is a different key, even when the prefix matches.
    REQUIRE(make_key({sql_value{std::string{"a"}}}) != make_key({sql_value{std::string{"a"}}, sql_value{std::string{}}}));
    REQUIRE(make_key({sql_value{std::string{"a"}}}) < make_key({sql_value{std::string{"a"}}, sql_value{std::string{}}}));

    REQUIRE(row_key{} == row_key{});
}

TEST_CASE("row_key orders canonical categories", "[key]")
{
    const row_key null_value = make_key({sql_value{}});
    const row_key false_value = make_key({sql_value{false}});
    const row_key true_value = make_key({sql_value{true}});
    const row_key number = int_key(-1);
    const row_key text = text_key("");
    const row_key blob = bytes_key(entityfy::bytes{});

    REQUIRE(null_value < false_value);
    REQUIRE(false_value < true_value);
    REQUIRE(true_value < number);
    REQUIRE(number < text);
    REQUIRE(text < blob);
}

TEST_CASE("row_key compares numbers by value", "[key]")
{
    // int64, uint64 and an integral double are the same numeric key value.
    REQUIRE(int_key(1) == uint_key(std::uint64_t{1}));
    REQUIRE(uint_key(std::uint64_t{1}) == real_key(1.0));
    REQUIRE(int_key(1) == real_key(1.0));

    // -1 < 0 < 1 across stored representations.
    REQUIRE(int_key(-1) < real_key(0.0));
    REQUIRE(real_key(0.0) < uint_key(std::uint64_t{1}));

    // INT64_MIN keeps its magnitude instead of overflowing to a positive value.
    REQUIRE(int_key(int64_min) < int_key(0));
    REQUIRE(int_key(int64_min) < int_key(int64_max));
    REQUIRE(int_key(int64_min) != uint_key(std::uint64_t{1} << 63));
    REQUIRE(int_key(int64_min) < uint_key(std::uint64_t{1} << 63));

    // A uint64 above INT64_MAX stays a large positive number.
    const auto above_int64 = static_cast<std::uint64_t>(int64_max) + 1;
    REQUIRE(uint_key(above_int64) != int_key(-1));
    REQUIRE(int_key(-1) < uint_key(above_int64));
    REQUIRE(uint_key(above_int64) > uint_key(static_cast<std::uint64_t>(int64_max)));

    // Non-integral reals keep their fractional part.
    REQUIRE(int_key(1) < real_key(1.5));
    REQUIRE(real_key(1.5) < int_key(2));

    // +0.0 and -0.0 are the same key value.
    const double negative_zero = -0.0;
    REQUIRE(std::signbit(negative_zero));
    REQUIRE(real_key(negative_zero) == real_key(0.0));
    REQUIRE(real_key(negative_zero) == int_key(0));
}

TEST_CASE("row_key treats NaN as one value after every number", "[key]")
{
    const double other_quiet_nan = std::bit_cast<double>(std::uint64_t{0x7FF8000000000001ULL});
    const double negative_quiet_nan = -quiet_nan;
    REQUIRE(std::signbit(negative_quiet_nan));

    // Every NaN bit pattern is the same key value, no matter the payload or sign.
    REQUIRE(real_key(quiet_nan) == real_key(other_quiet_nan));
    REQUIRE(real_key(quiet_nan) == real_key(negative_quiet_nan));
    REQUIRE(hash_of(real_key(quiet_nan)) == hash_of(real_key(other_quiet_nan)));

    // NaN sorts after every number and before the next category.
    REQUIRE(real_key(quiet_nan) > uint_key(std::numeric_limits<std::uint64_t>::max()));
    REQUIRE(real_key(quiet_nan) > int_key(int64_min));
    REQUIRE(real_key(quiet_nan) > real_key(1.5));
    REQUIRE(real_key(quiet_nan) < text_key(""));
}

TEST_CASE("row_key exposes a total, stable order", "[key]")
{
    const std::vector<row_key> ascending{
        row_key{},
        make_key({sql_value{}}),
        make_key({sql_value{false}}),
        make_key({sql_value{true}}),
        int_key(int64_min),
        int_key(-1),
        real_key(-0.0),
        uint_key(std::uint64_t{1}),
        real_key(1.5),
        int_key(int64_max),
        uint_key(std::uint64_t{1} << 63),
        real_key(quiet_nan),
        text_key(""),
        text_key("a"),
        bytes_key(entityfy::bytes{}),
        bytes_key(entityfy::bytes{std::byte{0x00}}),
    };

    for(std::size_t left = 0; left < ascending.size(); ++left)
    {
        for(std::size_t right = left + 1; right < ascending.size(); ++right)
        {
            REQUIRE(ascending[left] != ascending[right]);
            REQUIRE(ascending[left] < ascending[right]);
            REQUIRE_FALSE(ascending[right] < ascending[left]);
            REQUIRE(ascending[right] > ascending[left]);
        }
    }

    // Ordering is antisymmetric on equal values too.
    REQUIRE(real_key(-0.0) <= int_key(0));
    REQUIRE(real_key(-0.0) >= int_key(0));
    REQUIRE_FALSE(real_key(-0.0) < int_key(0));
}

TEST_CASE("row_key_hash agrees with equality across value forms", "[key]")
{
    REQUIRE(hash_of(int_key(1)) == hash_of(uint_key(std::uint64_t{1})));
    REQUIRE(hash_of(uint_key(std::uint64_t{1})) == hash_of(real_key(1.0)));
    REQUIRE(hash_of(real_key(-0.0)) == hash_of(int_key(0)));
    REQUIRE(hash_of(int_key(int64_min)) == hash_of(int_key(int64_min)));

    // The split-collision pair must not share a hash either.
    REQUIRE(hash_of(make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}})) !=
            hash_of(make_key({sql_value{std::string{"ab"}}, sql_value{std::string{"c"}}})));

    std::unordered_map<row_key, int, entityfy::row_key_hash> index;
    index[int_key(1)] = 10;
    index[make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}})] = 20;
    index[bytes_key(entityfy::bytes{std::byte{0x01}})] = 30;

    REQUIRE(index.size() == 3);

    // Equal keys written through another numeric form find the stored entry.
    REQUIRE(index.contains(real_key(1.0)));
    REQUIRE(index.at(uint_key(std::uint64_t{1})) == 10);
    REQUIRE(index.contains(make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}})));

    REQUIRE_FALSE(index.contains(make_key({sql_value{std::string{"ab"}}, sql_value{std::string{"c"}}})));
    REQUIRE_FALSE(index.contains(int_key(2)));
}

TEST_CASE("row_key to_string is diagnostic text only", "[key]")
{
    const row_key split_left = make_key({sql_value{std::string{"a"}}, sql_value{std::string{"bc"}}});
    const row_key split_right = make_key({sql_value{std::string{"ab"}}, sql_value{std::string{"c"}}});

    REQUIRE(split_left.to_string() == "(\"a\", \"bc\")");
    REQUIRE(split_right.to_string() == "(\"ab\", \"c\")");
    REQUIRE(split_left.to_string() != split_right.to_string());

    // The rendered text never feeds equality: the pair stays two values.
    REQUIRE(split_left != split_right);
    REQUIRE(hash_of(split_left) != hash_of(split_right));

    REQUIRE(make_key({sql_value{}, sql_value{true}, sql_value{std::int64_t{7}}, sql_value{std::string{"x"}}}).to_string() ==
            "(NULL, true, 7, \"x\")");
    REQUIRE(bytes_key(entityfy::bytes{std::byte{0x00}, std::byte{0xFF}}).to_string() == "(x'00ff')");
    REQUIRE(text_key("a\"b\n").to_string() == "(\"a\\\"b\\n\")");
}

TEST_CASE("row_key single-part keys keep the stored value", "[key]")
{
    const row_key key{{sql_value{std::int64_t{7}}}};

    REQUIRE(key.size() == 1);
    REQUIRE(key.parts().size() == 1);
    REQUIRE(std::holds_alternative<std::int64_t>(key.part(0)));
    REQUIRE(std::get<std::int64_t>(key.part(0)) == 7);
    REQUIRE_FALSE(std::holds_alternative<std::uint64_t>(key.part(0)));
    REQUIRE_FALSE(std::holds_alternative<double>(key.part(0)));
    REQUIRE(key == int_key(7));

    REQUIRE_THROWS_AS(static_cast<void>(key.part(1)), std::out_of_range);
}
