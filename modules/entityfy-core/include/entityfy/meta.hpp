#pragma once

#include <cstddef>
#include <concepts>
#include <memory>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace entityfy::meta
{

struct field
{
    std::string_view name;
};

template <typename Type>
class type
{
public:
    consteval static std::meta::info info()
    {
        return ^^Type;
    }

    consteval static auto name()
    {
        return std::define_static_string(std::meta::identifier_of(info()));
    }

    consteval static auto fields()
    {
        constexpr auto context = std::meta::access_context::current();
        auto           fields = std::meta::nonstatic_data_members_of(info(), context);
        return std::define_static_array(fields);
    }

    template <typename Object, typename Function>
    constexpr static void for_each_field(Object&& object, Function&& function)
    {
        using meta_object_type = std::remove_cvref_t<Object>;

        template for(constexpr auto meta_field: type<meta_object_type>::fields())
        {
            constexpr auto identifier = std::meta::identifier_of(meta_field);
            function(field{std::define_static_string(identifier)}, object.[:meta_field:]);
        }
    }
};

template <auto ReflectedField>
struct field_constant
{
    static constexpr auto value = ReflectedField;
};

template <typename Type>
consteval auto type_name()
{
    return type<std::remove_cvref_t<Type>>::name();
}

template <typename Type>
consteval std::meta::info type_info()
{
    return ^^Type;
}

template <auto ReflectedField>
consteval auto field_name()
{
    return std::define_static_string(std::meta::identifier_of(ReflectedField));
}

template <typename Model, auto ReflectedField>
using field_type_t = std::remove_cvref_t<decltype(std::declval<std::remove_cvref_t<Model>&>().[:ReflectedField:])>;

template <typename Type>
consteval std::size_t field_count()
{
    return type<std::remove_cvref_t<Type>>::fields().size();
}

template <typename Type>
consteval bool has_field_named(std::string_view name)
{
    bool found = false;

    template for(constexpr auto meta_field: type<std::remove_cvref_t<Type>>::fields())
    {
        if(std::meta::identifier_of(meta_field) == name)
        {
            found = true;
        }
    }

    return found;
}

template <typename Annotation>
consteval bool has_annotation(std::meta::info meta_ref)
{
    return !std::meta::annotations_of_with_type(meta_ref, ^^Annotation).empty();
}

template <typename Annotation, auto Reflected>
consteval bool has_annotation()
{
    return has_annotation<Annotation>(Reflected);
}

template <typename Annotation>
consteval Annotation annotation_or(std::meta::info meta_ref, Annotation fallback = {})
{
    auto annotations = std::meta::annotations_of_with_type(meta_ref, ^^Annotation);
    if(annotations.empty())
    {
        return fallback;
    }

    return std::meta::extract<Annotation>(annotations.front());
}

template <typename Annotation, auto Reflected>
consteval Annotation annotation_or(Annotation fallback = {})
{
    return annotation_or<Annotation>(Reflected, fallback);
}

template <typename Annotation, typename Type>
consteval Annotation type_annotation_or(Annotation fallback = {})
{
    return annotation_or<Annotation>(type_info<std::remove_cvref_t<Type>>(), fallback);
}

template <typename Type, typename Function>
constexpr void for_each_field_reflection(Function&& function)
{
    using ReflectedType = std::remove_cvref_t<Type>;

    template for(constexpr auto meta_field: type<ReflectedType>::fields())
    {
        function.template operator()<meta_field>();
    }
}

template <typename Type, typename IgnoreAnnotation, typename Function>
constexpr void for_each_persistent_field_reflection(Function&& function)
{
    for_each_field_reflection<std::remove_cvref_t<Type>>([&]<auto meta_field>() {
        if constexpr(!has_annotation<IgnoreAnnotation, meta_field>())
        {
            function.template operator()<meta_field>();
        }
    });
}

template <typename Type, typename IgnoreAnnotation>
consteval std::size_t persistent_field_count()
{
    std::size_t count = 0;

    for_each_persistent_field_reflection<std::remove_cvref_t<Type>, IgnoreAnnotation>([&]<auto>() {
        ++count;
    });

    return count;
}

template <typename IgnoreAnnotation, typename Object, typename Function>
constexpr void for_each_persistent_field(Object&& object, Function&& function)
{
    using ReflectedType = std::remove_cvref_t<Object>;

    for_each_persistent_field_reflection<ReflectedType, IgnoreAnnotation>([&]<auto meta_field>() {
        constexpr auto identifier = std::meta::identifier_of(meta_field);
        function(field{std::define_static_string(identifier)}, object.[:meta_field:]);
    });
}

template <typename IgnoreAnnotation, typename Model, typename Member, typename Resolver>
[[nodiscard]] std::string resolve_member_column(Member Model::* member, Resolver&& resolver)
{
    Model       probe{};
    std::string resolved;

    for_each_persistent_field_reflection<Model, IgnoreAnnotation>([&]<auto meta_field>() {
        using current_field_type = field_type_t<Model, meta_field>;

        if constexpr(std::same_as<current_field_type, std::remove_cvref_t<Member>>)
        {
            if(std::addressof(probe.*member) == std::addressof(probe.[:meta_field:]))
            {
                resolved = resolver.template operator()<meta_field>();
            }
        }
    });

    return resolved;
}

template <typename Object, typename Function>
constexpr void for_each_field(Object&& object, Function&& function)
{
    type<std::remove_cvref_t<Object>>::for_each_field(
        std::forward<Object>(object),
        [&](field meta_field, auto& value) {
            function(meta_field, value);
        }
    );
}

} // namespace entityfy::meta
