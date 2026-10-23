#pragma once

#include <entityfy/annotations.hpp>
#include <entityfy/dialect.hpp>
#include <entityfy/meta.hpp>
#include <entityfy/value.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace entityfy
{

struct column_flags
{
    bool primary_key = false;
    bool auto_increment = false;
    bool unique = false;
    bool indexed = false;
    bool not_null = false;
    bool nullable = false;
    bool ignored = false;
    bool created_at = false;
    bool updated_at = false;
};

struct reference_descriptor
{
    std::string table;
    std::string column;
    std::string on_delete;
    std::string on_update;
};

struct column_descriptor
{
    std::string          name{};
    column_flags         flags{};
    std::string          sql_type{};
    std::string          default_sql{};
    std::string          check_sql{};
    std::string          on_update_sql{};
    reference_descriptor reference{};
    bool                 has_reference = false;
    bool                 generated_on_insert = false;
};

struct relation_descriptor
{
    std::string local_column;
    std::string referenced_table;
    std::string referenced_column;
    std::string on_delete;
    std::string on_update;
};

namespace detail
{

template <typename Type>
consteval annotation_text annotated_table_name()
{
    const auto annotation = meta::type_annotation_or<table, Type>(table{});
    if(annotation.name.empty())
    {
        return annotation_text{meta::type_name<Type>()};
    }

    return annotation.name;
}

template <auto ReflectedField>
consteval annotation_text annotated_column_name()
{
    const auto annotation = meta::annotation_or<column, ReflectedField>(column{});
    if(annotation.name.empty())
    {
        return annotation_text{meta::field_name<ReflectedField>()};
    }

    return annotation.name;
}

// Column name as a static C string, honoring [[= entityfy::column{...}]].
// The returned pointer points at static storage, so the view stays valid.
template <auto ReflectedField>
consteval const char* static_column_name()
{
    const auto annotation = meta::annotation_or<column, ReflectedField>(column{});
    if(!annotation.name.empty())
    {
        return std::define_static_string(annotation.name.view());
    }

    return std::define_static_string(std::meta::identifier_of(ReflectedField));
}

// Walks the persisted columns of a model in lake order. An [[= entityfy::owned]]
// member contributes one column per persisted leaf field of its own type,
// named "<member>_<field>"; nested owned members keep extending that prefix.
// The callback receives <meta_field, field_type>(prefix, is_root).
template <typename Type, typename Function>
constexpr void for_each_column_reflection(Function&& function, std::string_view prefix = {}, bool root = true)
{
    using model_type = std::remove_cvref_t<Type>;

    template for(constexpr auto meta_field: meta::type<model_type>::fields())
    {
        if constexpr(!meta::has_annotation<ignore_t, meta_field>())
        {
            using field_type = meta::field_type_t<model_type, meta_field>;

            if constexpr(meta::has_annotation<owned_t, meta_field>())
            {
                const std::string nested_prefix =
                    std::string{prefix} + std::string{static_column_name<meta_field>()} + "_";
                for_each_column_reflection<field_type>(function, nested_prefix, false);
            }
            else
            {
                function.template operator()<meta_field, field_type>(prefix, root);
            }
        }
    }
}

// Runtime twin of for_each_column_reflection: visits the same leaves in the same
// order and hands out (prefix, leaf name, value). The name is a static string.
template <typename Object, typename Function>
constexpr void for_each_leaf_field(Object&& object, Function&& function, std::string_view prefix = {})
{
    using object_type = std::remove_cvref_t<Object>;

    template for(constexpr auto meta_field: meta::type<object_type>::fields())
    {
        if constexpr(!meta::has_annotation<ignore_t, meta_field>())
        {
            if constexpr(meta::has_annotation<owned_t, meta_field>())
            {
                const std::string nested_prefix =
                    std::string{prefix} + std::string{static_column_name<meta_field>()} + "_";
                for_each_leaf_field(object.[:meta_field:], function, nested_prefix);
            }
            else
            {
                function(prefix, std::string_view{static_column_name<meta_field>()}, object.[:meta_field:]);
            }
        }
    }
}

template <typename Field, auto ReflectedField>
consteval column_flags annotated_column_flags()
{
    column_flags flags{
        .not_null = !is_optional_v<Field>,
        .nullable = is_optional_v<Field>,
    };

    flags.primary_key = meta::has_annotation<primary_key_t, ReflectedField>();
    flags.auto_increment = meta::has_annotation<auto_increment_t, ReflectedField>();
    flags.unique = meta::has_annotation<unique_t, ReflectedField>();
    flags.indexed = meta::has_annotation<indexed_t, ReflectedField>();
    flags.ignored = meta::has_annotation<ignore_t, ReflectedField>();
    flags.created_at = meta::has_annotation<created_at_t, ReflectedField>();
    flags.updated_at = meta::has_annotation<updated_at_t, ReflectedField>();

    if(flags.auto_increment)
    {
        flags.primary_key = true;
    }

    if(meta::has_annotation<nullable_t, ReflectedField>())
    {
        flags.nullable = true;
        flags.not_null = false;
    }

    if(meta::has_annotation<not_null_t, ReflectedField>() || flags.primary_key)
    {
        flags.not_null = true;
        flags.nullable = false;
    }

    return flags;
}

template <typename Type>
struct column_storage_type
{
    using type = std::remove_cvref_t<Type>;
};

template <typename Type>
struct column_storage_type<std::optional<Type>>
{
    using type = std::remove_cvref_t<Type>;
};

template <typename Type>
using column_storage_type_t = typename column_storage_type<std::remove_cvref_t<Type>>::type;

template <typename Type>
inline constexpr bool is_auto_increment_storage_v =
    std::integral<column_storage_type_t<Type>> &&
    !std::same_as<column_storage_type_t<Type>, bool> &&
    !is_optional_v<Type>;

template <typename Type>
inline constexpr bool is_string_storage_v = std::same_as<column_storage_type_t<Type>, std::string>;

template <typename Type>
inline constexpr bool is_date_storage_v = is_string_storage_v<Type> || is_sys_days_v<column_storage_type_t<Type>>;

template <typename Type>
inline constexpr bool is_time_storage_v = is_string_storage_v<Type> || is_chrono_duration_v<column_storage_type_t<Type>>;

template <typename Type>
inline constexpr bool is_timestamp_storage_v =
    is_string_storage_v<Type> ||
    (is_system_time_point_v<column_storage_type_t<Type>> && !is_sys_days_v<column_storage_type_t<Type>>);

template <typename Type>
[[nodiscard]] std::string sql_type_for(dialect target)
{
    using value_type = std::remove_cvref_t<Type>;

    if constexpr(is_optional_v<value_type>)
    {
        return sql_type_for<optional_value_t<value_type>>(target);
    }
    else if constexpr(std::same_as<value_type, bool>)
    {
        return target == dialect::postgresql ? "BOOLEAN" : "INTEGER";
    }
    else if constexpr(std::is_enum_v<value_type>)
    {
        return "INTEGER";
    }
    else if constexpr(std::signed_integral<value_type> || std::unsigned_integral<value_type>)
    {
        if constexpr(sizeof(value_type) <= sizeof(std::int32_t))
        {
            return "INTEGER";
        }
        else
        {
            return target == dialect::postgresql ? "BIGINT" : "INTEGER";
        }
    }
    else if constexpr(std::floating_point<value_type>)
    {
        return target == dialect::postgresql ? "DOUBLE PRECISION" : "REAL";
    }
    else if constexpr(std::same_as<value_type, std::string>)
    {
        return "TEXT";
    }
    else if constexpr(is_sys_days_v<value_type>)
    {
        return target == dialect::postgresql ? "DATE" : "TEXT";
    }
    else if constexpr(is_system_time_point_v<value_type>)
    {
        return target == dialect::postgresql ? "TIMESTAMP" : "TEXT";
    }
    else if constexpr(is_chrono_duration_v<value_type>)
    {
        return target == dialect::postgresql ? "TIME" : "TEXT";
    }
    else if constexpr(is_byte_vector_v<value_type>)
    {
        return target == dialect::postgresql ? "BYTEA" : "BLOB";
    }
    else if constexpr(has_value_converter<value_type>)
    {
        // Types that use a converter are stored in a text column by default;
        // narrow them with [[= entityfy::text]] / [[= entityfy::varchar{n}}].
        return "TEXT";
    }
    else
    {
        static_assert(dependent_false<value_type>, "entityfy ORM has no SQL type mapping for this field type.");
    }
}

template <typename Field, auto ReflectedField>
[[nodiscard]] std::string annotated_sql_type_for(dialect target)
{
    const auto explicit_type = meta::annotation_or<sql_type, ReflectedField>(sql_type{});
    if(!explicit_type.value.empty())
    {
        return std::string{explicit_type.value.view()};
    }

    if constexpr(meta::has_annotation<varchar, ReflectedField>())
    {
        const auto annotation = meta::annotation_or<varchar, ReflectedField>();
        return "VARCHAR(" + std::to_string(annotation.length) + ")";
    }
    else if constexpr(meta::has_annotation<decimal, ReflectedField>())
    {
        const auto annotation = meta::annotation_or<decimal, ReflectedField>();
        return "DECIMAL(" + std::to_string(annotation.precision) + ", " + std::to_string(annotation.scale) + ")";
    }
    else if constexpr(meta::has_annotation<text_t, ReflectedField>())
    {
        return "TEXT";
    }
    else if constexpr(meta::has_annotation<blob_t, ReflectedField>())
    {
        return target == dialect::postgresql ? "BYTEA" : "BLOB";
    }
    else if constexpr(meta::has_annotation<json_t, ReflectedField>())
    {
        return target == dialect::postgresql ? "JSONB" : "TEXT";
    }
    else if constexpr(meta::has_annotation<uuid_t, ReflectedField>())
    {
        return target == dialect::postgresql ? "UUID" : "TEXT";
    }
    else if constexpr(meta::has_annotation<date_t, ReflectedField>())
    {
        return target == dialect::postgresql ? "DATE" : "TEXT";
    }
    else if constexpr(meta::has_annotation<time_t, ReflectedField>())
    {
        return target == dialect::postgresql ? "TIME" : "TEXT";
    }
    else if constexpr(
        meta::has_annotation<timestamp_t, ReflectedField>() ||
        meta::has_annotation<created_at_t, ReflectedField>() ||
        meta::has_annotation<updated_at_t, ReflectedField>()
    )
    {
        using storage_type = column_storage_type_t<Field>;

        if constexpr(is_sys_days_v<storage_type>)
        {
            return target == dialect::postgresql ? "DATE" : "TEXT";
        }
        else if constexpr(is_chrono_duration_v<storage_type>)
        {
            return target == dialect::postgresql ? "TIME" : "TEXT";
        }
        else
        {
            return target == dialect::postgresql ? "TIMESTAMP" : "TEXT";
        }
    }
    else
    {
        return sql_type_for<Field>(target);
    }
}

template <typename Field, auto ReflectedField>
[[nodiscard]] std::string current_time_sql()
{
    using storage_type = column_storage_type_t<Field>;

    if constexpr(meta::has_annotation<date_t, ReflectedField>() || is_sys_days_v<storage_type>)
    {
        return "CURRENT_DATE";
    }
    else if constexpr(meta::has_annotation<time_t, ReflectedField>() || is_chrono_duration_v<storage_type>)
    {
        return "CURRENT_TIME";
    }
    else
    {
        return "CURRENT_TIMESTAMP";
    }
}

template <typename Field, auto ReflectedField>
[[nodiscard]] std::string default_sql_for()
{
    const auto explicit_default = meta::annotation_or<default_value, ReflectedField>(default_value{});
    if(!explicit_default.sql.empty())
    {
        return std::string{explicit_default.sql.view()};
    }

    if constexpr(meta::has_annotation<created_at_t, ReflectedField>() || meta::has_annotation<updated_at_t, ReflectedField>())
    {
        return current_time_sql<Field, ReflectedField>();
    }

    return {};
}

inline std::string quote_identifier(std::string_view identifier)
{
    std::string quoted;
    quoted.reserve(identifier.size() + 2);
    quoted.push_back('"');

    for(const char character: identifier)
    {
        if(character == '"')
        {
            quoted.push_back('"');
        }

        quoted.push_back(character);
    }

    quoted.push_back('"');
    return quoted;
}

template <typename Type>
consteval std::size_t persistent_field_count()
{
    return meta::persistent_field_count<std::remove_cvref_t<Type>, ignore_t>();
}

template <typename Type>
consteval std::size_t explicit_primary_key_count()
{
    std::size_t count = 0;

    meta::for_each_persistent_field_reflection<std::remove_cvref_t<Type>, ignore_t>([&]<auto meta_field>() {
        if constexpr(meta::has_annotation<primary_key_t, meta_field>() || meta::has_annotation<auto_increment_t, meta_field>())
        {
            ++count;
        }
    });

    return count;
}

template <typename Type>
consteval bool has_implicit_id_key()
{
    bool found = false;

    meta::for_each_persistent_field_reflection<std::remove_cvref_t<Type>, ignore_t>([&]<auto meta_field>() {
        if(meta::field_name<meta_field>() == std::string_view{"id"})
        {
            found = true;
        }
    });

    return found;
}

template <typename Type>
consteval void validate_owned_struct();

// Scalar (non-owned) field: the annotations map one member to one column.
template <typename Model, auto ReflectedField>
consteval void validate_scalar_field_annotations()
{
    using field_type = meta::field_type_t<Model, ReflectedField>;
    using storage_type = column_storage_type_t<field_type>;

    static_assert(
        is_supported_field_v<field_type>,
        "entityfy ORM unsupported persisted field type. Supported columns are bool, integral, floating point, enum, std::string, std::vector<std::byte>, std::chrono::sys_days, std::chrono::system_clock::time_point, chrono durations, std::optional<T> of those types, and custom types with an entityfy::value_converter<T>. Use [[= entityfy::ignore]] for transient fields."
    );

    static_assert(
        !(meta::has_annotation<nullable_t, ReflectedField>() && meta::has_annotation<not_null_t, ReflectedField>()),
        "entityfy ORM field cannot be both [[= entityfy::nullable]] and [[= entityfy::not_null]]."
    );

    if constexpr(meta::has_annotation<primary_key_t, ReflectedField>())
    {
        static_assert(!is_optional_v<field_type>, "entityfy ORM primary keys cannot be std::optional<T>.");
    }

    if constexpr(meta::has_annotation<auto_increment_t, ReflectedField>())
    {
        static_assert(
            is_auto_increment_storage_v<field_type>,
            "entityfy ORM [[= entityfy::auto_increment]] is only valid on a non-optional integral field, excluding bool."
        );
        static_assert(
            !meta::has_annotation<default_value, ReflectedField>(),
            "entityfy ORM [[= entityfy::auto_increment]] cannot be combined with [[= entityfy::default_value{...}]]."
        );
    }

    if constexpr(meta::has_annotation<sql_type, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<sql_type, ReflectedField>();
        static_assert(!annotation.value.empty(), "entityfy ORM [[= entityfy::sql_type{...}]] requires a SQL type.");
    }

    if constexpr(meta::has_annotation<default_value, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<default_value, ReflectedField>();
        static_assert(!annotation.sql.empty(), "entityfy ORM [[= entityfy::default_value{...}]] requires a SQL expression.");
    }

    if constexpr(meta::has_annotation<references, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<references, ReflectedField>();
        static_assert(!annotation.table.empty(), "entityfy ORM [[= entityfy::references{...}]] requires a referenced table.");
    }

    if constexpr(meta::has_annotation<date_t, ReflectedField>())
    {
        static_assert(is_date_storage_v<field_type>, "entityfy ORM [[= entityfy::date]] requires std::string or std::chrono::sys_days.");
    }

    if constexpr(meta::has_annotation<time_t, ReflectedField>())
    {
        static_assert(is_time_storage_v<field_type>, "entityfy ORM [[= entityfy::time]] requires std::string or a std::chrono duration.");
    }

    if constexpr(meta::has_annotation<timestamp_t, ReflectedField>())
    {
        static_assert(
            is_timestamp_storage_v<field_type>,
            "entityfy ORM [[= entityfy::timestamp]] requires std::string or std::chrono::system_clock::time_point."
        );
    }

    if constexpr(meta::has_annotation<created_at_t, ReflectedField>() || meta::has_annotation<updated_at_t, ReflectedField>())
    {
        static_assert(
            is_timestamp_storage_v<field_type> || is_date_storage_v<field_type> || is_time_storage_v<field_type>,
            "entityfy ORM [[= entityfy::created_at]] and [[= entityfy::updated_at]] require a temporal field."
        );
        static_assert(!meta::has_annotation<primary_key_t, ReflectedField>(), "entityfy ORM timestamp automation is not valid on a primary key field.");
    }

    if constexpr(meta::has_annotation<uuid_t, ReflectedField>())
    {
        static_assert(is_string_storage_v<field_type>, "entityfy ORM [[= entityfy::uuid]] requires std::string.");
    }

    if constexpr(meta::has_annotation<json_t, ReflectedField>())
    {
        static_assert(
            is_string_storage_v<field_type> || has_value_converter<field_type>,
            "entityfy ORM [[= entityfy::json]] requires std::string JSON storage or a type with an entityfy::value_converter<T> that produces valid JSON."
        );
    }

    if constexpr(meta::has_annotation<text_t, ReflectedField>() || meta::has_annotation<varchar, ReflectedField>())
    {
        static_assert(
            is_string_storage_v<field_type> || has_value_converter<field_type>,
            "entityfy ORM text/varchar annotations require std::string or a type with an entityfy::value_converter<T>."
        );
    }

    if constexpr(meta::has_annotation<blob_t, ReflectedField>())
    {
        static_assert(is_byte_vector_v<storage_type>, "entityfy ORM [[= entityfy::blob]] requires std::vector<std::byte>.");
    }

    if constexpr(meta::has_annotation<varchar, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<varchar, ReflectedField>();
        static_assert(annotation.length > 0, "entityfy ORM varchar length must be positive.");
    }

    if constexpr(meta::has_annotation<decimal, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<decimal, ReflectedField>();
        static_assert(
            std::floating_point<storage_type> || is_string_storage_v<field_type>,
            "entityfy ORM [[= entityfy::decimal{...}]] requires a floating point field or std::string decimal storage."
        );
        static_assert(annotation.precision > 0, "entityfy ORM decimal precision must be positive.");
        static_assert(annotation.scale >= 0, "entityfy ORM decimal scale cannot be negative.");
        static_assert(annotation.scale <= annotation.precision, "entityfy ORM decimal scale cannot exceed precision.");
    }

    if constexpr(meta::has_annotation<check, ReflectedField>())
    {
        constexpr auto annotation = meta::annotation_or<check, ReflectedField>();
        static_assert(!annotation.sql.empty(), "entityfy ORM [[= entityfy::check{...}]] requires a SQL expression.");
    }
}

template <typename Model, auto ReflectedField>
consteval void validate_field_annotations()
{
    using field_type = meta::field_type_t<Model, ReflectedField>;

    if constexpr(meta::has_annotation<owned_t, ReflectedField>())
    {
        static_assert(
            !is_optional_v<field_type>,
            "entityfy ORM [[= entityfy::owned]] requires a plain struct member; wrap the flattened columns instead of the "
            "owned object."
        );
        validate_owned_struct<field_type>();
    }
    else
    {
        validate_scalar_field_annotations<Model, ReflectedField>();
    }
}

// Value object stored across several columns of the owning table.
template <typename Type>
consteval void validate_owned_struct()
{
    using owned_type = std::remove_cvref_t<Type>;

    static_assert(
        std::default_initializable<owned_type>,
        "entityfy ORM owned types must be default-initializable so rows can be materialized."
    );
    static_assert(
        meta::field_count<owned_type>() > 0,
        "entityfy ORM owned types must contain at least one persistent non-static data member."
    );
    static_assert(
        persistent_field_count<owned_type>() > 0,
        "entityfy ORM owned types must contain at least one persisted field; remove entityfy::ignore from one field."
    );

    meta::for_each_persistent_field_reflection<owned_type, ignore_t>([]<auto meta_field>() {
        static_assert(
            !meta::has_annotation<primary_key_t, meta_field>(),
            "entityfy ORM owned types cannot declare a primary key; the owning model owns the key."
        );
        static_assert(
            !meta::has_annotation<auto_increment_t, meta_field>(),
            "entityfy ORM owned types cannot declare an auto-increment column."
        );
        static_assert(
            !meta::has_annotation<created_at_t, meta_field>() && !meta::has_annotation<updated_at_t, meta_field>(),
            "entityfy ORM owned types cannot declare created_at/updated_at; put timestamps on the owning model."
        );
        validate_field_annotations<owned_type, meta_field>();
    });
}

template <typename Type>
consteval std::size_t flattened_column_count()
{
    std::size_t count = 0;

    for_each_column_reflection<std::remove_cvref_t<Type>>(
        [&]<auto, typename>(std::string_view, bool) {
            ++count;
        }
    );

    return count;
}

template <typename Type>
consteval void validate_model()
{
    using model_type = std::remove_cvref_t<Type>;

    static_assert(std::default_initializable<model_type>, "entityfy ORM models must be default-initializable so rows can be materialized.");
    static_assert(meta::field_count<model_type>() > 0, "entityfy ORM models must contain at least one persistent non-static data member.");
    static_assert(persistent_field_count<model_type>() > 0, "entityfy ORM models must contain at least one persisted field; remove entityfy::ignore from one field.");

    constexpr auto explicit_primary_keys = explicit_primary_key_count<model_type>();
    static_assert(explicit_primary_keys <= 1, "entityfy ORM models must contain at most one primary key annotation.");

    if constexpr(explicit_primary_keys == 0)
    {
        static_assert(
            has_implicit_id_key<model_type>(),
            "entityfy ORM models need a primary key. Add [[= entityfy::primary_key]] to one field or provide a persisted `id` field."
        );
    }

    meta::for_each_persistent_field_reflection<model_type, ignore_t>([]<auto meta_field>() {
        validate_field_annotations<model_type, meta_field>();
    });
}

struct static_column_metadata
{
    annotation_text  name;
    column_flags     flags;
    default_value    default_sql;
    check            check_sql;
    references       reference;
};

template <typename Field, auto ReflectedField>
consteval static_column_metadata static_describe_field()
{
    return static_column_metadata{
        .name = annotated_column_name<ReflectedField>(),
        .flags = annotated_column_flags<Field, ReflectedField>(),
        .default_sql = meta::annotation_or<default_value, ReflectedField>(default_value{}),
        .check_sql = meta::annotation_or<check, ReflectedField>(check{}),
        .reference = meta::annotation_or<references, ReflectedField>(references{}),
    };
}

} // namespace detail

template <typename Type>
[[nodiscard]] std::string table_name()
{
    detail::validate_model<Type>();
    return std::string{detail::annotated_table_name<std::remove_cvref_t<Type>>().view()};
}

template <typename Type>
[[nodiscard]] std::vector<column_descriptor> describe(dialect target)
{
    using model_type = std::remove_cvref_t<Type>;
    detail::validate_model<model_type>();

    std::vector<column_descriptor> descriptors;
    descriptors.reserve(detail::flattened_column_count<model_type>());

    constexpr auto explicit_primary_keys = detail::explicit_primary_key_count<model_type>();

    detail::for_each_column_reflection<model_type>([&]<auto meta_field, typename field_type>(
        std::string_view prefix, bool is_root) {
        constexpr auto metadata = detail::static_describe_field<field_type, meta_field>();

        auto descriptor = column_descriptor{
            .name = std::string{prefix} + std::string{metadata.name.view()},
            .flags = metadata.flags,
            .sql_type = detail::annotated_sql_type_for<field_type, meta_field>(target),
            .default_sql = detail::default_sql_for<field_type, meta_field>(),
        };

        if(!metadata.check_sql.sql.empty())
        {
            descriptor.check_sql = std::string{metadata.check_sql.sql.view()};
        }

        if(!metadata.reference.table.empty())
        {
            descriptor.has_reference = true;
            descriptor.reference = reference_descriptor{
                .table = std::string{metadata.reference.table.view()},
                .column = std::string{metadata.reference.column.empty() ? std::string_view{"id"} : metadata.reference.column.view()},
                .on_delete = std::string{metadata.reference.on_delete.view()},
                .on_update = std::string{metadata.reference.on_update.view()},
            };
        }

        if(descriptor.flags.updated_at)
        {
            descriptor.on_update_sql = detail::current_time_sql<field_type, meta_field>();
        }

        descriptor.generated_on_insert =
            descriptor.flags.auto_increment ||
            descriptor.flags.created_at ||
            descriptor.flags.updated_at;

        if constexpr(explicit_primary_keys == 0)
        {
            if(is_root && std::string_view{meta::field_name<meta_field>()} == "id")
            {
                descriptor.flags.primary_key = true;
                descriptor.flags.not_null = true;
                descriptor.flags.nullable = false;
            }
        }

        descriptors.emplace_back(std::move(descriptor));
    });

    return descriptors;
}

template <typename Type>
struct model_descriptor
{
    dialect                        target{};
    std::string                    table_name{};
    std::vector<column_descriptor> columns{};
    std::vector<relation_descriptor> relations{};
};

template <typename Type>
[[nodiscard]] model_descriptor<std::remove_cvref_t<Type>> describe_model(dialect target)
{
    using model_type = std::remove_cvref_t<Type>;

    auto columns = describe<model_type>(target);
    std::vector<relation_descriptor> relations;

    for(const auto& column: columns)
    {
        if(column.has_reference)
        {
            relations.emplace_back(relation_descriptor{
                .local_column = column.name,
                .referenced_table = column.reference.table,
                .referenced_column = column.reference.column,
                .on_delete = column.reference.on_delete,
                .on_update = column.reference.on_update,
            });
        }
    }

    return model_descriptor<model_type>{
        .target = target,
        .table_name = table_name<model_type>(),
        .columns = std::move(columns),
        .relations = std::move(relations),
    };
}

template <typename Type>
[[nodiscard]] column_descriptor primary_key_column(const model_descriptor<Type>& model)
{
    auto found = std::ranges::find_if(model.columns, [](const column_descriptor& descriptor) {
        return descriptor.flags.primary_key;
    });

    if(found == model.columns.end())
    {
        throw std::logic_error{"entityfy ORM schema validation failed to resolve a primary key"};
    }

    return *found;
}

template <typename Type>
[[nodiscard]] column_descriptor primary_key_column(dialect target)
{
    auto descriptors = describe<Type>(target);
    auto found = std::ranges::find_if(descriptors, [](const column_descriptor& descriptor) {
        return descriptor.flags.primary_key;
    });

    if(found == descriptors.end())
    {
        throw std::logic_error{"entityfy ORM schema validation failed to resolve a primary key"};
    }

    return *found;
}

template <typename Model, typename Member>
[[nodiscard]] bool member_is_owned(Member Model::* member)
{
    Model probe{};
    bool  owned = false;

    meta::for_each_field_reflection<Model>([&]<auto meta_field>() {
        using current_field_type = meta::field_type_t<Model, meta_field>;

        if constexpr(std::same_as<current_field_type, std::remove_cvref_t<Member>>)
        {
            if(std::addressof(probe.*member) == std::addressof(probe.[:meta_field:]))
            {
                owned = meta::has_annotation<owned_t, meta_field>();
            }
        }
    });

    return owned;
}

template <typename Model, typename Member>
[[nodiscard]] std::string column_name(Member Model::* member)
{
    detail::validate_model<Model>();

    if(member_is_owned(member))
    {
        throw std::logic_error{
            "entityfy ORM column_name() received an owned field; an owned field spans several columns, so name them as strings"
        };
    }

    const auto resolved = meta::resolve_member_column<ignore_t, Model>(member, []<auto meta_field>() {
        return std::string{detail::annotated_column_name<meta_field>().view()};
    });

    if(resolved.empty())
    {
        throw std::logic_error{"entityfy::where(&T::field) received a member pointer that is not part of the persisted model"};
    }

    return resolved;
}

} // namespace entityfy
