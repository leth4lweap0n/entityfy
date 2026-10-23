#pragma once

#include <cstddef>
#include <string_view>

namespace entityfy
{

struct annotation_text
{
    static constexpr std::size_t max_size = 256;

    char        value[max_size + 1]{};
    std::size_t length = 0;

    consteval annotation_text() = default;

    template <std::size_t Size>
    consteval annotation_text(const char (&input)[Size])
        : length{Size - 1}
    {
        static_assert(Size <= max_size + 1, "entityfy ORM annotation string exceeds 256 characters.");

        for(std::size_t index = 0; index < Size; ++index)
        {
            value[index] = input[index];
        }
    }

    consteval annotation_text(std::string_view input)
        : length{input.size()}
    {
        if(input.size() > max_size)
        {
            throw "entityfy ORM annotation string exceeds 256 characters.";
        }

        for(std::size_t index = 0; index < input.size(); ++index)
        {
            value[index] = input[index];
        }
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return length == 0;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {value, length};
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept
    {
        return view();
    }
};

struct table
{
    annotation_text name;
};

struct column
{
    annotation_text name;
};

struct sql_type
{
    annotation_text value;
};

struct default_value
{
    annotation_text sql;
};

struct references
{
    annotation_text table;
    annotation_text column = "id";
    annotation_text on_delete;
    annotation_text on_update;
};

struct varchar
{
    int length = 255;
};

struct decimal
{
    int precision = 65;
    int scale = 30;
};

struct check
{
    annotation_text sql;
};

struct primary_key_t
{
};

struct auto_increment_t
{
};

struct unique_t
{
};

struct indexed_t
{
};

struct not_null_t
{
};

struct nullable_t
{
};

struct ignore_t
{
};

// Marks the member used for optimistic concurrency checks. Any integral,
// persisted member works; without the annotation the member named "version"
// keeps working.
struct concurrency_token_t
{
};

// Maps the member onto PostgreSQL's `xmin` system column and makes it the
// optimistic concurrency token. PostgreSQL only: the column is never created,
// inserted or assigned, reads come from `xmin` and the value is refreshed
// after every write. Use a plain integral member (std::uint32_t is natural).
struct xmin_t
{
};

struct created_at_t
{
};

struct updated_at_t
{
};

struct date_t
{
};

struct time_t
{
};

struct timestamp_t
{
};

struct uuid_t
{
};

struct json_t
{
};

struct text_t
{
};

struct blob_t
{
};

// Marks a struct-typed member whose own persisted fields are flattened into the
// owning table with a "<member>_" prefix, e.g. `[[= entityfy::owned]] address address;`
// persists `address_city`, `address_zip`, ...
struct owned_t
{
};

// TPH: tags the string column that stores the concrete type of a row.
struct discriminator_t
{
};

// TPH: declares the discriminator value written by a derived model, e.g.
// `struct [[= entityfy::type_tag{"cat"}]] cat : animal { ... };`
struct type_tag
{
    annotation_text value;
};

using map = column;
using db_type = sql_type;
using default_sql = default_value;
using numeric = decimal;

inline constexpr primary_key_t    primary_key{};
inline constexpr primary_key_t    id{};
inline constexpr auto_increment_t auto_increment{};
inline constexpr unique_t         unique{};
inline constexpr indexed_t        indexed{};
inline constexpr indexed_t        index{};
inline constexpr not_null_t       not_null{};
inline constexpr nullable_t       nullable{};
inline constexpr ignore_t         ignore{};
inline constexpr concurrency_token_t concurrency_token{};
inline constexpr xmin_t              xmin{};
inline constexpr ignore_t         ignored{};
inline constexpr created_at_t     created_at{};
inline constexpr updated_at_t     updated_at{};
inline constexpr date_t           date{};
inline constexpr time_t           time{};
inline constexpr timestamp_t      timestamp{};
inline constexpr uuid_t           uuid{};
inline constexpr json_t           json{};
inline constexpr text_t           text{};
inline constexpr blob_t           blob{};
inline constexpr owned_t          owned{};
inline constexpr discriminator_t  discriminator{};

} // namespace entityfy
